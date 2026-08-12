#include "media/MediaProxy.h"

#include <ruvia/core/OneShot.h>

#include <asio/connect.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <istream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace service::gb28181 {
namespace {

constexpr std::size_t kMaximumResponseHeadBytes = 64U * 1024U;
constexpr std::size_t kReadChunkBytes = 32U * 1024U;
constexpr auto kUpstreamOperationTimeout = std::chrono::seconds(30);

std::string trim(std::string value) {
  while (!value.empty() &&
         (value.back() == '\r' || value.back() == '\n' ||
          std::isspace(static_cast<unsigned char>(value.back())) != 0))
    value.pop_back();
  auto begin = value.begin();
  while (begin != value.end() &&
         std::isspace(static_cast<unsigned char>(*begin)) != 0)
    ++begin;
  value.erase(value.begin(), begin);
  return value;
}

std::string lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool forwardedResponseHeader(std::string_view name) noexcept {
  return name == "content-type" || name == "cache-control" ||
         name == "content-range" || name == "accept-ranges" || name == "etag" ||
         name == "last-modified" || name == "access-control-allow-origin" ||
         name == "access-control-expose-headers";
}

std::string requestText(std::uint16_t port, const MediaProxyRequest &request) {
  std::ostringstream output;
  output << request.method << ' ' << request.target << " HTTP/1.0\r\n"
         << "Host: 127.0.0.1:" << port << "\r\n"
         << "Connection: close\r\n"
         << "User-Agent: iot-engine-media-proxy/1\r\n";
  if (!request.accept.empty())
    output << "Accept: " << request.accept << "\r\n";
  if (!request.range.empty())
    output << "Range: " << request.range << "\r\n";
  if (!request.body.empty()) {
    output << "Content-Type: "
           << (request.contentType.empty() ? "application/octet-stream"
                                           : request.contentType)
           << "\r\nContent-Length: " << request.body.size() << "\r\n";
  }
  output << "\r\n" << request.body;
  return output.str();
}

} // namespace

struct MediaProxySession::Impl final
    : public std::enable_shared_from_this<MediaProxySession::Impl> {
  Impl(ruvia::EventLoop eventLoop, std::uint16_t port,
       MediaProxyRequest request)
      : loop(std::move(eventLoop)), socket(loop.ioContext()),
        deadline(loop.ioContext()), response(kMaximumResponseHeadBytes),
        requestData(requestText(port, request)), port(port) {}

  void request(ruvia::OneShotCompletion<MediaProxyChunk> completion) {
    if (pending.has_value()) {
      (void)completion.complete(MediaProxyChunk{
          .error = "concurrent media proxy read is not allowed"});
      return;
    }
    if (closed) {
      (void)completion.complete(MediaProxyChunk{.eof = true});
      return;
    }
    pending.emplace(std::move(completion));
    armDeadline();
    if (!connected)
      connect();
    else
      readBody();
  }

  void armDeadline() {
    deadline.expires_after(kUpstreamOperationTimeout);
    auto self = shared_from_this();
    deadline.async_wait([self](const std::error_code &error) {
      if (error || !self->pending)
        return;
      self->fail("upstream timeout", asio::error::timed_out);
    });
  }

  void connect() {
    auto self = shared_from_this();
    const auto endpoint =
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port);
    socket.async_connect(endpoint, [self](const std::error_code &error) {
      if (error)
        return self->fail("connect", error);
      self->connected = true;
      self->writeRequest();
    });
  }

  void writeRequest() {
    auto self = shared_from_this();
    asio::async_write(socket, asio::buffer(requestData),
                      [self](const std::error_code &error, std::size_t) {
                        if (error)
                          return self->fail("write", error);
                        self->readHead();
                      });
  }

  void readHead() {
    auto self = shared_from_this();
    asio::async_read_until(socket, response, "\r\n\r\n",
                           [self](const std::error_code &error, std::size_t) {
                             if (error)
                               return self->fail("read response head", error);
                             self->deliverHead();
                           });
  }

  void deliverHead() {
    MediaProxyChunk chunk;
    chunk.first = true;
    std::istream input(&response);
    std::string line;
    if (!std::getline(input, line)) {
      fail("parse response head", asio::error::invalid_argument);
      return;
    }
    std::istringstream statusLine(line);
    std::string protocol;
    statusLine >> protocol >> chunk.head.status;
    if (!protocol.starts_with("HTTP/") || chunk.head.status < 100 ||
        chunk.head.status > 599) {
      fail("parse response status", asio::error::invalid_argument);
      return;
    }
    while (std::getline(input, line)) {
      line = trim(std::move(line));
      if (line.empty())
        break;
      const auto separator = line.find(':');
      if (separator == std::string::npos)
        continue;
      auto name = lower(trim(line.substr(0, separator)));
      auto value = trim(line.substr(separator + 1));
      if (forwardedResponseHeader(name) && !value.empty())
        chunk.head.headers.emplace_back(std::move(name), std::move(value));
    }
    const auto remaining = response.size();
    if (remaining != 0) {
      chunk.data.resize(remaining);
      input.read(chunk.data.data(), static_cast<std::streamsize>(remaining));
    }
    answer(std::move(chunk));
  }

  void readBody() {
    auto self = shared_from_this();
    socket.async_read_some(
        asio::buffer(readBuffer),
        [self](const std::error_code &error, std::size_t bytes) {
          if (error == asio::error::eof) {
            self->closed = true;
            self->answer(MediaProxyChunk{.eof = true});
            return;
          }
          if (error)
            return self->fail("read response body", error);
          MediaProxyChunk chunk;
          chunk.data.assign(self->readBuffer.data(), bytes);
          self->answer(std::move(chunk));
        });
  }

  void fail(std::string_view operation, const std::error_code &error) {
    closed = true;
    std::error_code ignored;
    socket.close(ignored);
    MediaProxyChunk chunk;
    chunk.error = std::string(operation) + ": " + error.message();
    answer(std::move(chunk));
  }

  void answer(MediaProxyChunk chunk) {
    if (!pending)
      return;
    std::error_code ignored;
    deadline.cancel(ignored);
    auto completion = std::move(*pending);
    pending.reset();
    const auto result = completion.complete(std::move(chunk));
    if (!result.accepted()) {
      closed = true;
      std::error_code ignored;
      socket.close(ignored);
    }
  }

  void cancel() {
    if (closed)
      return;
    closed = true;
    std::error_code ignored;
    deadline.cancel(ignored);
    socket.cancel(ignored);
    socket.close(ignored);
    if (pending)
      answer(MediaProxyChunk{.error = "media proxy request cancelled"});
  }

  ruvia::EventLoop loop;
  asio::ip::tcp::socket socket;
  asio::steady_timer deadline;
  asio::streambuf response;
  std::string requestData;
  std::uint16_t port{};
  std::array<char, kReadChunkBytes> readBuffer{};
  std::optional<ruvia::OneShotCompletion<MediaProxyChunk>> pending;
  bool connected{false};
  bool closed{false};
};

MediaProxySession::MediaProxySession(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

std::shared_ptr<MediaProxySession>
MediaProxySession::create(ruvia::EventLoop loop, std::uint16_t port,
                          MediaProxyRequest request) {
  if (!loop.valid() || port == 0)
    throw std::runtime_error("GB28181 media proxy is unavailable");
  auto implementation =
      std::make_shared<Impl>(std::move(loop), port, std::move(request));
  auto result = std::shared_ptr<MediaProxySession>(
      new MediaProxySession(std::move(implementation)));
  return result;
}

ruvia::Task<MediaProxyChunk>
MediaProxySession::next(ruvia::WorkerHandle worker) {
  auto [completion, receiver] =
      ruvia::makeOneShot<MediaProxyChunk>(std::move(worker));
  auto implementation = impl_;
  const auto posted = impl_->loop.post(
      [implementation, completion = std::move(completion)]() mutable {
        implementation->request(std::move(completion));
      });
  if (!posted.accepted())
    throw std::runtime_error("GB28181 media proxy worker is stopping");
  auto waited = co_await receiver.wait();
  if (waited.hasValue())
    co_return std::move(waited).takeValue();
  throw std::runtime_error("GB28181 media proxy request was cancelled");
}

void MediaProxySession::cancel() noexcept {
  if (!impl_)
    return;
  auto implementation = impl_;
  (void)impl_->loop.post([implementation] { implementation->cancel(); });
}

namespace media_proxy_detail {

bool allowedTarget(std::string_view method, std::string_view path) noexcept {
  if (method == "POST")
    return path == "/index/api/webrtc";
  if (method != "GET" || !path.starts_with("/rtp/") ||
      path.find("..") != std::string_view::npos)
    return false;
  return std::ranges::none_of(path, [](unsigned char character) {
    return character <= 0x20U || character == 0x7FU || character == '\\' ||
           character == '%' || character == '?' || character == '#';
  });
}

std::string rewriteHlsPlaylist(std::string_view playlist,
                               std::string_view query) {
  if (query.empty())
    return std::string(playlist);
  std::string output;
  output.reserve(playlist.size() + query.size() * 8U);
  while (!playlist.empty()) {
    const auto end = playlist.find('\n');
    auto line =
        end == std::string_view::npos ? playlist : playlist.substr(0, end);
    const auto carriage = line.ends_with('\r');
    if (carriage)
      line.remove_suffix(1);
    if (!line.empty() && !line.starts_with('#')) {
      output.append(line);
      output.push_back(line.find('?') == std::string_view::npos ? '?' : '&');
      output.append(query);
    } else {
      output.append(line);
    }
    if (carriage)
      output.push_back('\r');
    if (end == std::string_view::npos)
      break;
    output.push_back('\n');
    playlist.remove_prefix(end + 1);
  }
  return output;
}

} // namespace media_proxy_detail

} // namespace service::gb28181
