#pragma once

#include <ruvia/web/ContextRequest.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/features/gb28181/runtime.h"

#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace service::gb28181 {

class MediaProxyController final
    : public ruvia::Controller<MediaProxyController> {
public:
  RUVIA_CONTROLLER_GROUP("/media")
  RUVIA_ROUTES_BEGIN
  RUVIA_GET_STREAM("/*", proxy);
  RuviaControllerAccess::addResponseStreamRoute(
      ruviaRouteScope, ruvia::HttpKnownMethod::kPost, "/*",
      RuviaControllerAccess::template bindStream<&RuviaControllerType::proxy>(
          this),
      RuviaControllerAccess::template makeMiddlewares<>());
  RUVIA_ROUTES_END

private:
  class SessionGuard final {
  public:
    explicit SessionGuard(std::shared_ptr<MediaProxySession> value)
        : value_(std::move(value)) {}
    ~SessionGuard() {
      if (value_)
        value_->cancel();
    }

  private:
    std::shared_ptr<MediaProxySession> value_;
  };

  static std::string percentEncode(std::string_view value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
      const auto byte = static_cast<unsigned char>(ch);
      if (std::isalnum(byte) != 0 || ch == '-' || ch == '_' || ch == '.' ||
          ch == '~') {
        result.push_back(ch);
      } else {
        result.push_back('%');
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
      }
    }
    return result;
  }

  static std::string queryString(const ruvia::ContextRequest &request) {
    std::string result;
    for (const auto &field : request.queryFields()) {
      if (!result.empty())
        result.push_back('&');
      result.append(percentEncode(field.name()));
      result.push_back('=');
      result.append(percentEncode(field.value()));
    }
    return result;
  }

  static void applyHead(ruvia::Context &context,
                        const MediaProxyResponseHead &head) {
    if (head.status < 200 || head.status > 599)
      service::common::fail(10002, "媒体服务返回了无效状态", 502);
    context.status(ruvia::HttpStatusCode::fromValue(
        static_cast<std::uint16_t>(head.status)));
    for (const auto &[name, value] : head.headers)
      context.header(name, value);
  }

  ruvia::Task<void> proxy(ruvia::Context &context) {
    if (!runtime().enabled() || !runtime().started())
      service::common::fail(10002, "GB28181 媒体服务未启用", 503);

    const auto request = context.req();
    const auto path = request.path();
    if (!path.starts_with("/media"))
      service::common::fail(10001, "媒体代理路径无效", 400);
    const auto upstreamPath = path.substr(6);
    if (!media_proxy_detail::allowedTarget(request.method(), upstreamPath))
      service::common::fail(10003, "媒体资源不存在", 404);

    const auto query = queryString(request);
    MediaProxyRequest upstream;
    upstream.method.assign(request.method());
    upstream.target.assign(upstreamPath);
    if (!query.empty()) {
      upstream.target.push_back('?');
      upstream.target.append(query);
    }
    if (const auto value = request.header("Content-Type"))
      upstream.contentType.assign(*value);
    if (const auto value = request.header("Accept"))
      upstream.accept.assign(*value);
    if (const auto value = request.header("Range"))
      upstream.range.assign(*value);
    if (upstream.method == "POST")
      upstream.body.assign(co_await request.text());

    auto session = runtime().openMediaProxy(std::move(upstream));
    SessionGuard guard(session);
    auto &writer = context.stream();
    const auto playlist = upstreamPath.ends_with(".m3u8");
    std::string playlistBody;
    bool headApplied = false;
    while (!writer.aborted()) {
      auto chunk = co_await session->next(context.worker());
      if (!chunk.error.empty()) {
        if (!headApplied)
          service::common::fail(10002, "媒体代理连接失败", 502);
        throw std::runtime_error("GB28181 media proxy: " + chunk.error);
      }
      if (chunk.first) {
        applyHead(context, chunk.head);
        headApplied = true;
      }
      if (!chunk.data.empty()) {
        if (playlist) {
          if (playlistBody.size() + chunk.data.size() > 2U * 1024U * 1024U)
            throw std::runtime_error("GB28181 HLS playlist is too large");
          playlistBody.append(chunk.data);
        } else {
          co_await writer.write(chunk.data);
        }
      }
      if (!chunk.eof)
        continue;
      if (!headApplied)
        service::common::fail(10002, "媒体服务未返回响应", 502);
      if (playlist && !playlistBody.empty()) {
        auto rewritten =
            media_proxy_detail::rewriteHlsPlaylist(playlistBody, query);
        co_await writer.write(rewritten);
      }
      co_await writer.end();
      co_return;
    }
    session->cancel();
  }
};

} // namespace service::gb28181
