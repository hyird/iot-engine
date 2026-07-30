#pragma once

#include "config/AppConfig.h"
#include "media/MediaTypes.h"

#include <mk_mediakit.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

class ZlmSdk final {
  public:
    struct Callbacks {
        std::function<void(std::string app, std::string stream, std::string schema,
                           bool online, int readerCount)>
            onStreamChanged;
        std::function<void(std::string app, std::string stream, std::string schema)>
            onStreamNoneReader;
        std::function<void(std::string stream)> onRtpDetached;
    };

    struct Ports {
        std::uint16_t http{0};
        std::uint16_t https{0};
        std::uint16_t rtsp{0};
        std::uint16_t rtsps{0};
        std::uint16_t rtmp{0};
        std::uint16_t rtmps{0};
        std::uint16_t rtc{0};
        std::uint16_t srt{0};
    };

    struct Capabilities {
        bool faac{true};
        bool ffmpeg{true};
        bool hls{true};
        bool mp4{true};
        bool rtpProxy{true};
        bool srt{true};
        bool sctp{true};
        bool webRtc{true};
        bool x264{true};
        bool videoStack{true};
        bool tls{false};
        bool recording{false};
    };

    explicit ZlmSdk(MediaConfig config, Callbacks callbacks = {});
    ~ZlmSdk();

    ZlmSdk(const ZlmSdk&) = delete;
    ZlmSdk& operator=(const ZlmSdk&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool started() const noexcept { return started_.load(); }
    [[nodiscard]] Ports ports() const noexcept { return ports_; }
    [[nodiscard]] Capabilities capabilities() const noexcept;

    [[nodiscard]] std::optional<OpenRtpServerResult>
    openRtpServer(const std::string& deviceId, const std::string& channelId,
                  const std::string& ssrc, const std::string& mode = "preview");
    [[nodiscard]] bool closeRtpServer(const std::string& streamId);
    [[nodiscard]] PlayUrls buildPlayUrls(const std::string& streamId) const;
    [[nodiscard]] bool validatePlayToken(std::string_view stream,
                                         std::string_view token,
                                         std::int64_t expires) const;
    [[nodiscard]] bool startMp4Recording(const std::string& streamId);
    [[nodiscard]] bool stopMp4Recording(const std::string& streamId);
    [[nodiscard]] bool isMp4Recording(const std::string& streamId) const;

  private:
    struct CallbackState {
        std::atomic_bool active{true};
        std::mutex invocationMutex;
        Callbacks callbacks;
        std::string playTokenSecret;
        std::string corsOrigin;
    };

    struct RtpCallbackState {
        std::weak_ptr<CallbackState> callbacks;
        std::string stream;
    };

    struct RtpServerDeleter {
        void operator()(std::remove_pointer_t<mk_rtp_server>* server) const noexcept;
    };

    using RtpServerPtr =
        std::unique_ptr<std::remove_pointer_t<mk_rtp_server>, RtpServerDeleter>;

    MediaConfig config_;
    Ports ports_;
    std::shared_ptr<CallbackState> callbacks_;
    std::atomic_bool started_{false};
    std::atomic_uint nextRtpPort_{0};
    mutable std::mutex rtpMutex_;
    std::unordered_map<std::string, RtpServerPtr> rtpServers_;

    static std::mutex callbackMutex_;
    static std::weak_ptr<CallbackState> activeCallbacks_;

    [[nodiscard]] std::string makeStreamId(const std::string& deviceId,
                                           const std::string& channelId,
                                           const std::string& ssrc,
                                           const std::string& mode) const;
    [[nodiscard]] std::uint16_t allocateRtpPort();
    [[nodiscard]] std::string sdkIni() const;
    [[nodiscard]] std::string makePlayToken(std::string_view stream,
                                            std::int64_t expires) const;

    static std::shared_ptr<CallbackState> activeCallbacks();
    static bool authorizePlay(const CallbackState& state, std::string_view stream,
                              std::string_view token, std::int64_t expires);
    static void API_CALL handleMediaChanged(int registered, const mk_media_source source);
    static void API_CALL handleMediaNoReader(const mk_media_source source);
    static void API_CALL handleMediaPlay(const mk_media_info media,
                                         const mk_auth_invoker invoker,
                                         const mk_sock_info sender);
    static void API_CALL handleHttpRequest(const mk_parser parser,
                                           const mk_http_response_invoker invoker,
                                           int* consumed, const mk_sock_info sender);
    static void API_CALL handleWebRtcAnswer(void* userData, const char* answer,
                                            const char* error);
    static void API_CALL releaseHttpInvoker(void* userData);
    static void API_CALL handleRtpDetached(void* userData);
    static void API_CALL releaseRtpCallback(void* userData);
};
