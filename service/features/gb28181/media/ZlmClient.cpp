#include "media/ZlmClient.h"

#include <boost/json.hpp>
#include <trantor/utils/Logger.h>
#include <drogon/drogon.h>

#include <sstream>
#include <utility>

namespace {

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string httpToWs(std::string value) {
    if (value.rfind("http://", 0) == 0) {
        value.replace(0, 4, "ws");
    } else if (value.rfind("https://", 0) == 0) {
        value.replace(0, 5, "wss");
    }
    return value;
}

std::string httpToRtsp(std::string value) {
    if (value.rfind("http://", 0) == 0) {
        value.replace(0, 4, "rtsp");
    } else if (value.rfind("https://", 0) == 0) {
        value.replace(0, 5, "rtsp");
    }
    const auto scheme = value.find("://");
    if (scheme != std::string::npos) {
        const auto hostStart = scheme + 3;
        const auto path = value.find('/', hostStart);
        auto authority = value.substr(hostStart, path == std::string::npos ? std::string::npos : path - hostStart);
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            authority = authority.substr(0, colon);
        }
        return value.substr(0, hostStart) + authority + ":8554";
    }
    return value;
}

std::optional<boost::json::object> parseObject(const std::string& body) {
    boost::system::error_code error;
    auto value = boost::json::parse(body, error);
    if (error || !value.is_object()) {
        return std::nullopt;
    }
    return value.as_object();
}

std::string compactForLog(std::string value, std::size_t limit = 512) {
    const auto truncated = value.size() > limit;
    if (truncated) {
        value.resize(limit);
    }

    std::string output;
    output.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '\r') {
            output += "\\r";
        } else if (ch == '\n') {
            output += "\\n";
        } else if (ch == '\t') {
            output += "\\t";
        } else {
            output.push_back(ch);
        }
    }
    if (truncated) {
        output += "...";
    }
    return output;
}

} // namespace

ZlmClient::ZlmClient(MediaConfig config, trantor::EventLoop* loop)
    : config_(std::move(config)), loop_(loop) {
    nextRtpPort_.store(config_.rtpPortRangeStart);
}

drogon::Task<std::optional<OpenRtpServerResult>> ZlmClient::openRtpServerCoro(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode) {
    const auto baseUrl = trimTrailingSlash(config_.zlmBaseUrl);
    const auto streamId = makeStreamId(deviceId, channelId, ssrc, mode);
    const auto requestedPort = allocateRtpPort();
    auto client = drogon::HttpClient::newHttpClient(baseUrl, loop_);

    std::ostringstream path;
    path << "/index/api/openRtpServer?secret=" << config_.zlmSecret
         << "&port=" << requestedPort
         << "&enable_tcp=1"
         << "&stream_id=" << streamId;

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(path.str());

    LOG_DEBUG << "[GB28181][ZLM] openRtpServer request, device=" << deviceId
             << ", channel=" << channelId
             << ", mode=" << mode
             << ", stream_id=" << streamId
             << ", ssrc=" << ssrc
             << ", requested_port=" << requestedPort
             << ", base_url=" << baseUrl;

    auto response = co_await client->sendRequestCoro(request, 3.0);
    if (response == nullptr) {
        LOG_WARN << "[GB28181][ZLM] openRtpServer request failed, stream_id=" << streamId
                 << ", requested_port=" << requestedPort;
        co_return std::nullopt;
    }

    LOG_DEBUG << "[GB28181][ZLM] openRtpServer response, stream_id=" << streamId
              << ", http_status=" << static_cast<int>(response->getStatusCode())
              << ", body=\"" << compactForLog(std::string(response->body()), 800) << "\"";

    const auto parsed = parseObject(std::string(response->body()));
    if (!parsed.has_value()) {
        LOG_WARN << "[GB28181][ZLM] openRtpServer returned invalid JSON, stream_id=" << streamId
                 << ", http_status=" << static_cast<int>(response->getStatusCode())
                 << ", body=\"" << compactForLog(std::string(response->body()), 800) << "\"";
        co_return std::nullopt;
    }

    const auto& object = *parsed;
    const auto code = object.if_contains("code");
    const auto port = object.if_contains("port");
    if (code == nullptr || !code->is_int64() || code->as_int64() != 0) {
        LOG_WARN << "[GB28181][ZLM] openRtpServer failed, stream_id=" << streamId
                 << ", http_status=" << static_cast<int>(response->getStatusCode())
                 << ", body=\"" << compactForLog(std::string(response->body()), 800) << "\"";
        co_return std::nullopt;
    }

    OpenRtpServerResult openResult;
    openResult.streamId = streamId;
    openResult.port = port != nullptr && port->is_int64() ? static_cast<uint16_t>(port->as_int64()) : requestedPort;
    openResult.playUrls = buildPlayUrls(streamId);
    LOG_DEBUG << "[GB28181][ZLM] openRtpServer success, stream_id=" << streamId
             << ", requested_port=" << requestedPort
             << ", actual_port=" << openResult.port
             << ", http_flv=" << openResult.playUrls.httpFlv
             << ", ws_flv=" << openResult.playUrls.wsFlv;
    co_return openResult;
}

drogon::Task<bool> ZlmClient::closeRtpServerCoro(const std::string& streamId) {
    const auto baseUrl = trimTrailingSlash(config_.zlmBaseUrl);
    auto client = drogon::HttpClient::newHttpClient(baseUrl, loop_);

    std::ostringstream path;
    path << "/index/api/closeRtpServer?secret=" << config_.zlmSecret
         << "&stream_id=" << streamId;

    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(path.str());

    LOG_DEBUG << "[GB28181][ZLM] closeRtpServer request, stream_id=" << streamId
             << ", base_url=" << baseUrl;

    auto response = co_await client->sendRequestCoro(request, 3.0);
    if (response == nullptr) {
        LOG_WARN << "[GB28181][ZLM] closeRtpServer request failed, stream_id=" << streamId;
        co_return false;
    }

    LOG_DEBUG << "[GB28181][ZLM] closeRtpServer response, stream_id=" << streamId
             << ", http_status=" << static_cast<int>(response->getStatusCode())
             << ", body=\"" << compactForLog(std::string(response->body()), 800) << "\"";
    co_return true;
}

PlayUrls ZlmClient::buildPlayUrls(const std::string& streamId) const {
    const auto baseUrl = trimTrailingSlash(config_.zlmPublicBaseUrl.empty() ? config_.zlmBaseUrl : config_.zlmPublicBaseUrl);
    const auto wsBaseUrl = httpToWs(baseUrl);
    const auto rtspBaseUrl = httpToRtsp(baseUrl);
    const auto app = std::string("rtp");

    return PlayUrls{
        baseUrl + "/" + app + "/" + streamId + ".live.flv",
        wsBaseUrl + "/" + app + "/" + streamId + ".live.flv",
        baseUrl + "/" + app + "/" + streamId + ".live.ts",
        baseUrl + "/" + app + "/" + streamId + "/hls.m3u8",
        baseUrl + "/index/api/webrtc?app=" + app + "&stream=" + streamId + "&type=play",
        rtspBaseUrl + "/" + app + "/" + streamId,
    };
}

std::string ZlmClient::makeStreamId(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode) const {
    const std::string prefix = mode == "playback" ? "gb_playback" : "gb";
    return prefix + "_" + deviceId + "_" + channelId + "_" + ssrc;
}

uint16_t ZlmClient::allocateRtpPort() {
    auto port = nextRtpPort_.fetch_add(2);
    if (port > config_.rtpPortRangeEnd || port < config_.rtpPortRangeStart) {
        nextRtpPort_.store(config_.rtpPortRangeStart + 2);
        port = config_.rtpPortRangeStart;
    }
    if (port % 2 != 0) {
        ++port;
    }
    return static_cast<uint16_t>(port);
}
