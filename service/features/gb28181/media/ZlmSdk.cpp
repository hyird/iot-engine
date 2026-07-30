#include "media/ZlmSdk.h"

#include "service/common/log.h"

#include <ruvia/web/ModelJson.h>

#include <algorithm>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::string_view kDefaultVhost{"__defaultVhost__"};
constexpr std::string_view kRtpApp{"rtp"};
constexpr std::string_view kWebRtcPath{"/index/api/webrtc"};

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string httpToWs(std::string value) {
    if (value.rfind("http://", 0) == 0)
        value.replace(0, 4, "ws");
    else if (value.rfind("https://", 0) == 0)
        value.replace(0, 5, "wss");
    return value;
}

std::string authorityHost(std::string_view url) {
    const auto scheme = url.find("://");
    const auto begin = scheme == std::string_view::npos ? 0 : scheme + 3;
    const auto end = url.find('/', begin);
    auto authority = url.substr(begin, end == std::string_view::npos ? url.size() - begin
                                                                    : end - begin);
    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        return close == std::string_view::npos ? std::string(authority)
                                               : std::string(authority.substr(0, close + 1));
    }
    const auto colon = authority.rfind(':');
    return std::string(colon == std::string_view::npos ? authority : authority.substr(0, colon));
}

std::string publicHttpBase(const MediaConfig& config, std::uint16_t actualPort) {
    if (!config.zlmPublicBaseUrl.empty())
        return trimTrailingSlash(config.zlmPublicBaseUrl);
    std::ostringstream value;
    value << "http://" << config.rtpPublicIp;
    if (actualPort != 80)
        value << ':' << actualPort;
    return value.str();
}

std::string rtspBase(const MediaConfig& config, std::uint16_t httpPort,
                     std::uint16_t rtspPort) {
    const auto host = authorityHost(publicHttpBase(config, httpPort));
    std::ostringstream value;
    value << "rtsp://" << host;
    if (rtspPort != 554)
        value << ':' << rtspPort;
    return value.str();
}

bool managedStream(std::string_view stream) {
    return stream.starts_with("gb_") || stream.starts_with("gb_playback_");
}

const char* safe(const char* value) { return value == nullptr ? "" : value; }

std::uint32_t parseSsrc(std::string_view value) {
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? result : 0;
}

} // namespace

std::mutex ZlmSdk::callbackMutex_;
std::weak_ptr<ZlmSdk::CallbackState> ZlmSdk::activeCallbacks_;

ZlmSdk::ZlmSdk(MediaConfig config, Callbacks callbacks)
    : config_(std::move(config)),
      callbacks_(std::make_shared<CallbackState>()) {
    callbacks_->callbacks = std::move(callbacks);
    nextRtpPort_.store(config_.rtpPortRangeStart);
}

ZlmSdk::~ZlmSdk() { stop(); }

void ZlmSdk::RtpServerDeleter::operator()(
    std::remove_pointer_t<mk_rtp_server>* server) const noexcept {
    if (server != nullptr)
        mk_rtp_server_release(server);
}

void ZlmSdk::start() {
    if (started_.exchange(true))
        return;

    try {
        if (config_.workerThreads < 0)
            throw std::runtime_error("ZLM worker thread count cannot be negative");

        {
            std::lock_guard lock(callbackMutex_);
            if (!activeCallbacks_.expired())
                throw std::runtime_error("Only one embedded ZLMediaKit runtime is supported");
            callbacks_->active.store(true);
            activeCallbacks_ = callbacks_;
        }

        const auto ini = sdkIni();
        mk_config sdkConfig{};
        sdkConfig.thread_num = config_.workerThreads;
        sdkConfig.log_level = config_.logLevel;
        sdkConfig.log_mask = LOG_CONSOLE;
        sdkConfig.log_file_path = nullptr;
        sdkConfig.log_file_days = 0;
        sdkConfig.ini_is_path = 0;
        sdkConfig.ini = ini.c_str();
        sdkConfig.ssl_is_path = 0;
        sdkConfig.ssl = nullptr;
        sdkConfig.ssl_pwd = nullptr;
        mk_env_init(&sdkConfig);

        mk_events events{};
        events.on_mk_media_changed = &ZlmSdk::handleMediaChanged;
        events.on_mk_media_no_reader = &ZlmSdk::handleMediaNoReader;
        events.on_mk_http_request = &ZlmSdk::handleHttpRequest;
        mk_events_listen(&events);

        ports_.http = mk_http_server_start(config_.httpPort, 0);
        ports_.rtsp = mk_rtsp_server_start(config_.rtspPort, 0);
        ports_.rtmp = mk_rtmp_server_start(config_.rtmpPort, 0);
        ports_.rtc = mk_rtc_server_start(config_.rtcPort);
        ports_.srt = mk_srt_server_start(config_.srtPort);
        if (ports_.http == 0 || ports_.rtsp == 0 || ports_.rtmp == 0 ||
            ports_.rtc == 0 || ports_.srt == 0)
            throw std::runtime_error("Embedded ZLMediaKit failed to bind one or more servers");

        LOG_INFO << "[GB28181][ZLM SDK] Started with isolated media workers, workers="
                 << config_.workerThreads << ", http=" << ports_.http
                 << ", rtsp=" << ports_.rtsp << ", rtmp=" << ports_.rtmp
                 << ", rtc=" << ports_.rtc << ", srt=" << ports_.srt;
    } catch (...) {
        stop();
        throw;
    }
}

void ZlmSdk::stop() noexcept {
    if (!started_.exchange(false))
        return;

    {
        std::lock_guard invocationLock(callbacks_->invocationMutex);
        callbacks_->active.store(false);
    }
    {
        std::lock_guard lock(callbackMutex_);
        const auto active = activeCallbacks_.lock();
        if (active == callbacks_)
            activeCallbacks_.reset();
    }
    mk_events_listen(nullptr);
    {
        std::lock_guard lock(rtpMutex_);
        rtpServers_.clear();
    }
    mk_stop_all_server();
    ports_ = {};
    LOG_INFO << "[GB28181][ZLM SDK] Stopped";
}

std::optional<OpenRtpServerResult>
ZlmSdk::openRtpServer(const std::string& deviceId, const std::string& channelId,
                      const std::string& ssrc, const std::string& mode) {
    if (!started_.load())
        return std::nullopt;

    const auto streamId = makeStreamId(deviceId, channelId, ssrc, mode);
    const auto randomPort =
        config_.rtpPortRangeStart == 0 && config_.rtpPortRangeEnd == 0;
    const auto attempts =
        randomPort
            ? 1U
            : std::max(
                  1U,
                  static_cast<unsigned int>(
                      (config_.rtpPortRangeEnd - config_.rtpPortRangeStart) / 2U + 1U));

    for (unsigned int attempt = 0; attempt < attempts; ++attempt) {
        const auto requestedPort = randomPort ? 0 : allocateRtpPort();
        try {
            auto* raw = mk_rtp_server_create2(requestedPort, 1, kDefaultVhost.data(),
                                              kRtpApp.data(), streamId.c_str());
            if (raw == nullptr)
                continue;

            RtpServerPtr server(raw);
            const auto parsedSsrc = parseSsrc(ssrc);
            if (parsedSsrc != 0)
                mk_rtp_server_update_ssrc(server.get(), parsedSsrc);

            auto* detachState =
                new RtpCallbackState{.callbacks = callbacks_, .stream = streamId};
            mk_rtp_server_set_on_detach2(server.get(), &ZlmSdk::handleRtpDetached,
                                         detachState, &ZlmSdk::releaseRtpCallback);

            const auto actualPort = mk_rtp_server_port(server.get());
            {
                std::lock_guard lock(rtpMutex_);
                rtpServers_.insert_or_assign(streamId, std::move(server));
            }

            LOG_INFO << "[GB28181][ZLM SDK] RTP server opened, stream=" << streamId
                     << ", port=" << actualPort << ", ssrc=" << ssrc;
            return OpenRtpServerResult{
                .streamId = streamId,
                .port = actualPort,
                .playUrls = buildPlayUrls(streamId),
            };
        } catch (const std::exception& error) {
            LOG_WARN << "[GB28181][ZLM SDK] RTP port bind failed, stream=" << streamId
                     << ", port=" << requestedPort << ", error=" << error.what();
        }
    }

    LOG_ERROR << "[GB28181][ZLM SDK] RTP port range exhausted, stream=" << streamId;
    return std::nullopt;
}

bool ZlmSdk::closeRtpServer(const std::string& streamId) {
    RtpServerPtr server;
    {
        std::lock_guard lock(rtpMutex_);
        const auto found = rtpServers_.find(streamId);
        if (found == rtpServers_.end())
            return false;
        server = std::move(found->second);
        rtpServers_.erase(found);
    }
    server.reset();
    LOG_INFO << "[GB28181][ZLM SDK] RTP server closed, stream=" << streamId;
    return true;
}

PlayUrls ZlmSdk::buildPlayUrls(const std::string& streamId) const {
    const auto httpBase = publicHttpBase(config_, ports_.http == 0 ? config_.httpPort
                                                                  : ports_.http);
    const auto wsBase = httpToWs(httpBase);
    const auto rtsp = rtspBase(config_, ports_.http == 0 ? config_.httpPort : ports_.http,
                               ports_.rtsp == 0 ? config_.rtspPort : ports_.rtsp);
    const auto path = std::string("/") + std::string(kRtpApp) + "/" + streamId;
    return PlayUrls{
        .httpFlv = httpBase + path + ".live.flv",
        .wsFlv = wsBase + path + ".live.flv",
        .httpTs = httpBase + path + ".live.ts",
        .hls = httpBase + path + "/hls.m3u8",
        .webRtc = httpBase + std::string(kWebRtcPath) + "?app=" +
                  std::string(kRtpApp) + "&stream=" + streamId + "&type=play",
        .rtsp = rtsp + path,
    };
}

std::string ZlmSdk::makeStreamId(const std::string& deviceId,
                                 const std::string& channelId, const std::string& ssrc,
                                 const std::string& mode) const {
    const std::string prefix = mode == "playback" ? "gb_playback" : "gb";
    return prefix + "_" + deviceId + "_" + channelId + "_" + ssrc;
}

std::uint16_t ZlmSdk::allocateRtpPort() {
    auto port = nextRtpPort_.fetch_add(2);
    if (port > config_.rtpPortRangeEnd || port < config_.rtpPortRangeStart) {
        nextRtpPort_.store(config_.rtpPortRangeStart + 2U);
        port = config_.rtpPortRangeStart;
    }
    if (port % 2U != 0)
        ++port;
    return static_cast<std::uint16_t>(port);
}

std::string ZlmSdk::sdkIni() const {
    std::ostringstream ini;
    ini << "[general]\n"
        << "streamNoneReaderDelayMS=20000\n"
        << "[protocol]\n"
        << "enable_hls=1\n"
        << "enable_hls_fmp4=1\n"
        << "enable_rtsp=1\n"
        << "enable_rtmp=1\n"
        << "enable_ts=1\n"
        << "enable_fmp4=1\n"
        << "[rtc]\n"
        << "externIP=" << config_.rtpPublicIp << "\n"
        << "port=" << config_.rtcPort << "\n"
        << "tcpPort=" << config_.rtcPort << "\n";
    return ini.str();
}

std::shared_ptr<ZlmSdk::CallbackState> ZlmSdk::activeCallbacks() {
    std::lock_guard lock(callbackMutex_);
    const auto state = activeCallbacks_.lock();
    return state && state->active.load() ? state : nullptr;
}

void API_CALL ZlmSdk::handleMediaChanged(int registered, const mk_media_source source) {
    const auto state = activeCallbacks();
    if (!state)
        return;
    std::lock_guard invocationLock(state->invocationMutex);
    if (!state->active.load() || !state->callbacks.onStreamChanged)
        return;
    state->callbacks.onStreamChanged(
        safe(mk_media_source_get_app(source)), safe(mk_media_source_get_stream(source)),
        safe(mk_media_source_get_schema(source)), registered != 0,
        mk_media_source_get_total_reader_count(source));
}

void API_CALL ZlmSdk::handleMediaNoReader(const mk_media_source source) {
    const auto stream = std::string(safe(mk_media_source_get_stream(source)));
    if (!managedStream(stream) || mk_media_source_get_total_reader_count(source) > 0)
        return;
    const auto state = activeCallbacks();
    if (!state)
        return;
    std::lock_guard invocationLock(state->invocationMutex);
    if (!state->active.load() || !state->callbacks.onStreamNoneReader)
        return;
    state->callbacks.onStreamNoneReader(safe(mk_media_source_get_app(source)),
                                        std::move(stream),
                                        safe(mk_media_source_get_schema(source)));
}

void API_CALL ZlmSdk::handleHttpRequest(const mk_parser parser,
                                        const mk_http_response_invoker invoker,
                                        int* consumed, const mk_sock_info) {
    if (consumed == nullptr)
        return;
    *consumed = 0;
    if (!activeCallbacks() ||
        std::string_view(safe(mk_parser_get_url(parser))) != kWebRtcPath)
        return;

    *consumed = 1;
    const auto app = std::string(safe(mk_parser_get_url_param(parser, "app")));
    const auto stream = std::string(safe(mk_parser_get_url_param(parser, "stream")));
    const auto type = std::string(safe(mk_parser_get_url_param(parser, "type")));
    std::size_t contentLength = 0;
    const auto* offer = mk_parser_get_content(parser, &contentLength);
    if (app != kRtpApp || !managedStream(stream) || type != "play" || offer == nullptr ||
        contentLength == 0) {
        const char* headers[] = {"Content-Type", "application/json",
                                 "Access-Control-Allow-Origin", "*", nullptr};
        WebRtcResponseModel body;
        body.code(-1).msg("invalid WebRTC play request");
        const auto serialized = ruvia::toJson(body);
        mk_http_response_invoker_do_string(invoker, 400, headers, serialized.c_str());
        return;
    }

    const auto rtcUrl =
        "rtc://" + std::string(kDefaultVhost) + "/" + app + "/" + stream;
    auto* cloned = mk_http_response_invoker_clone(invoker);
    mk_webrtc_get_answer_sdp2(cloned, &ZlmSdk::releaseHttpInvoker,
                              &ZlmSdk::handleWebRtcAnswer, type.c_str(), offer,
                              rtcUrl.c_str());
}

void API_CALL ZlmSdk::handleWebRtcAnswer(void* userData, const char* answer,
                                         const char* error) {
    const auto invoker = static_cast<mk_http_response_invoker>(userData);
    const char* headers[] = {"Content-Type", "application/json",
                             "Access-Control-Allow-Origin", "*", nullptr};
    WebRtcResponseModel body;
    if (answer != nullptr) {
        body.code(0).type("answer").sdp(answer);
    } else {
        body.code(-1).msg(safe(error));
    }
    const auto serialized = ruvia::toJson(body);
    mk_http_response_invoker_do_string(invoker, 200, headers, serialized.c_str());
}

void API_CALL ZlmSdk::releaseHttpInvoker(void* userData) {
    if (userData != nullptr)
        mk_http_response_invoker_clone_release(
            static_cast<mk_http_response_invoker>(userData));
}

void API_CALL ZlmSdk::handleRtpDetached(void* userData) {
    const auto* detached = static_cast<RtpCallbackState*>(userData);
    if (detached == nullptr)
        return;
    const auto state = detached->callbacks.lock();
    if (!state)
        return;
    std::lock_guard invocationLock(state->invocationMutex);
    if (state->active.load() && state->callbacks.onRtpDetached)
        state->callbacks.onRtpDetached(detached->stream);
}

void API_CALL ZlmSdk::releaseRtpCallback(void* userData) {
    delete static_cast<RtpCallbackState*>(userData);
}
