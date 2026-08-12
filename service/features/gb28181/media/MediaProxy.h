#pragma once

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace service::gb28181 {

struct MediaProxyRequest final {
  std::string method;
  std::string target;
  std::string contentType;
  std::string accept;
  std::string range;
  std::string body;
};

struct MediaProxyResponseHead final {
  unsigned status{502};
  std::vector<std::pair<std::string, std::string>> headers;
};

struct MediaProxyChunk final {
  bool first{false};
  bool eof{false};
  MediaProxyResponseHead head;
  std::string data;
  std::string error;
};

class MediaProxySession final
    : public std::enable_shared_from_this<MediaProxySession> {
public:
  static std::shared_ptr<MediaProxySession>
  create(ruvia::EventLoop loop, std::uint16_t port, MediaProxyRequest request);

  MediaProxySession(const MediaProxySession &) = delete;
  MediaProxySession &operator=(const MediaProxySession &) = delete;

  ruvia::Task<MediaProxyChunk> next(ruvia::WorkerHandle worker);
  void cancel() noexcept;

private:
  struct Impl;

  explicit MediaProxySession(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

namespace media_proxy_detail {

[[nodiscard]] bool allowedTarget(std::string_view method,
                                 std::string_view path) noexcept;
[[nodiscard]] std::string rewriteHlsPlaylist(std::string_view playlist,
                                             std::string_view query);

} // namespace media_proxy_detail

} // namespace service::gb28181
