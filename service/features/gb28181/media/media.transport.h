#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mk_mediakit.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <ruvia/core/EventLoop.h>
#include <ruvia/web/ContextRequest.h>
#include <ruvia/web/Controller.h>

#include "service/features/gb28181/gb28181.config.h"
#include "service/features/gb28181/media/media.types.h"

class ZlmSdk final {
  public:
    using OwnerIndex = std::size_t;
    static constexpr OwnerIndex kUnassignedOwner =
        std::numeric_limits<OwnerIndex>::max();

    // ZLMediaKit invokes callbacks from its own worker threads.  The SIP
    // actor installs this scope while it services a request so a newly
    // created media session can retain the caller's Service Worker owner.
    class OwnerScope final {
      public:
        explicit OwnerScope(std::optional<OwnerIndex> owner) noexcept;
        ~OwnerScope();

        OwnerScope(const OwnerScope&) = delete;
        OwnerScope& operator=(const OwnerScope&) = delete;

      private:
        OwnerIndex previous_{ kUnassignedOwner };
    };

    struct Callbacks {
        std::function<void(std::string app, std::string stream, std::string schema, bool online, int readerCount)>
            onStreamChanged;
        std::function<void(std::string stream)> onRtpDetached;
        std::function<void(std::string app, std::string stream, std::string schema, bool online, int readerCount, OwnerIndex owner)>
            onStreamChangedOwned;
        std::function<void(std::string stream, OwnerIndex owner)>
            onRtpDetachedOwned;
        std::function<void(std::string stream, OwnerIndex owner)>
            onStreamOwner;
    };

    struct Ports {
        std::uint16_t http{ 0 };
        std::uint16_t https{ 0 };
        std::uint16_t rtsp{ 0 };
        std::uint16_t rtsps{ 0 };
        std::uint16_t rtmp{ 0 };
        std::uint16_t rtmps{ 0 };
        std::uint16_t rtc{ 0 };
        std::uint16_t srt{ 0 };
    };

    struct Capabilities {
        bool faac{ true };
        bool ffmpeg{ true };
        bool hls{ true };
        bool mp4{ true };
        bool rtpProxy{ true };
        bool srt{ true };
        bool sctp{ true };
        bool webRtc{ true };
        bool x264{ true };
        bool videoStack{ true };
        bool tls{ false };
        bool recording{ false };
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
    openRtpServer(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode = "preview");
    [[nodiscard]] bool closeRtpServer(const std::string& streamId);
    [[nodiscard]] PlayUrls buildPlayUrls(const std::string& streamId) const;
    [[nodiscard]] bool validatePlayToken(std::string_view stream, std::string_view token, std::int64_t expires) const;
    [[nodiscard]] bool startMp4Recording(const std::string& streamId);
    [[nodiscard]] bool stopMp4Recording(const std::string& streamId);
    [[nodiscard]] bool isMp4Recording(const std::string& streamId) const;

  private:
    struct CallbackState {
        std::atomic_bool active{ true };
        std::mutex invocationMutex;
        Callbacks callbacks;
        std::string playTokenSecret;
        std::string corsOrigin;
        std::unordered_map<std::string, OwnerIndex> streamOwners;
    };

    struct RtpCallbackState {
        std::weak_ptr<CallbackState> callbacks;
        std::string stream;
        OwnerIndex owner{ kUnassignedOwner };
    };

    struct RtpServerDeleter {
        void operator()(std::remove_pointer_t<mk_rtp_server>* server) const noexcept;
    };

    using RtpServerPtr =
        std::unique_ptr<std::remove_pointer_t<mk_rtp_server>, RtpServerDeleter>;

    MediaConfig config_;
    Ports ports_;
    std::shared_ptr<CallbackState> callbacks_;
    std::atomic_bool started_{ false };
    std::atomic_uint nextRtpPort_{ 0 };
    mutable std::mutex rtpMutex_;
    std::unordered_map<std::string, RtpServerPtr> rtpServers_;

    static std::mutex callbackMutex_;
    static std::weak_ptr<CallbackState> activeCallbacks_;
    static thread_local OwnerIndex actorOwner_;

    [[nodiscard]] std::string makeStreamId(const std::string& deviceId, const std::string& channelId, const std::string& ssrc, const std::string& mode) const;
    [[nodiscard]] std::uint16_t allocateRtpPort();
    [[nodiscard]] std::string sdkIni() const;
    [[nodiscard]] std::string makePlayToken(std::string_view stream, std::int64_t expires) const;

    [[nodiscard]] OwnerIndex streamOwner(std::string_view stream) const;

    static std::shared_ptr<CallbackState> activeCallbacks();
    static bool authorizePlay(const CallbackState& state, std::string_view stream, std::string_view token, std::int64_t expires);
    static void API_CALL handleMediaChanged(int registered, const mk_media_source source);
    static void API_CALL handleMediaPlay(const mk_media_info media, const mk_auth_invoker invoker, const mk_sock_info sender);
    static void API_CALL handleHttpRequest(const mk_parser parser, const mk_http_response_invoker invoker, int* consumed, const mk_sock_info sender);
    static void API_CALL handleWebRtcAnswer(void* userData, const char* answer, const char* error);
    static void API_CALL releaseHttpInvoker(void* userData);
    static void API_CALL handleRtpDetached(void* userData);
    static void API_CALL releaseRtpCallback(void* userData);
};

// ZLMediaKit owns a process-global callback table and its own worker threads.
// The SDK is therefore kept behind one explicit adapter supervisor.  Business
// state never lives here: a registered Collector only supplies callbacks that
// are posted back to the Collector's own Ruvia event loop.
class SdkSupervisor final {
  public:
    using OwnerIndex = ZlmSdk::OwnerIndex;

    struct CollectorCallbacks final {
        std::function<void(std::string app, std::string stream, std::string schema, bool online, int readerCount)>
            onStreamChanged;
        std::function<void(std::string stream)> onRtpDetached;
    };

    SdkSupervisor() = default;
    SdkSupervisor(const SdkSupervisor&) = delete;
    SdkSupervisor& operator=(const SdkSupervisor&) = delete;

    void configure(MediaConfig config);
    void start();
    void stop() noexcept;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool started() const noexcept;

    [[nodiscard]] const MediaConfig& config() const noexcept { return config_; }

    [[nodiscard]] ZlmSdk& sdk();
    [[nodiscard]] const ZlmSdk* sdkIfStarted() const noexcept;

    void registerCollector(OwnerIndex owner, ruvia::EventLoop loop, CollectorCallbacks callbacks);
    void unregisterCollector(OwnerIndex owner) noexcept;

    [[nodiscard]] ZlmSdk::Ports ports() const noexcept;
    [[nodiscard]] ZlmSdk::Capabilities capabilities() const noexcept;

  private:
    struct Route;

    static SdkSupervisor& instance();
    friend SdkSupervisor& sdkSupervisor();

    [[nodiscard]] std::shared_ptr<Route> routeFor(OwnerIndex owner) const;

    // Serializes SDK lifecycle transitions.  ZLMediaKit has process-global
    // callbacks, so configure/start/stop and Collector registration must not
    // overlap even though individual route lookups remain lock-free with
    // respect to this lifecycle mutex.
    mutable std::mutex lifecycleMutex_;
    mutable std::mutex mutex_;
    MediaConfig config_;
    std::unique_ptr<ZlmSdk> sdk_;
    std::unordered_map<OwnerIndex, std::shared_ptr<Route>> routes_;
    bool configured_{ false };

    [[nodiscard]] std::unique_ptr<ZlmSdk> makeSdk() const;
};

// There is one SDK adapter because ZLMediaKit exposes one process-global
// runtime.  This accessor is deliberately limited to SDK lifecycle/media
// transport; Collector protocol and session state remains per Collector.
SdkSupervisor& sdkSupervisor();

#include <utility>
#include <vector>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace service::gb28181 {

namespace media_proxy_detail {

[[nodiscard]] bool allowedTarget(std::string_view method, std::string_view path) noexcept;
[[nodiscard]] std::string rewriteHlsPlaylist(std::string_view playlist, std::string_view query);

} // namespace media_proxy_detail

class MediaProxyController final
    : public ruvia::Controller<MediaProxyController> {
  public:
    RUVIA_CONTROLLER_GROUP("/media")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_STREAM("/*", proxy);
    RuviaControllerAccess::addResponseStreamRoute(
        ruviaRouteScope,
        ruvia::HttpKnownMethod::kPost,
        "/*",
        RuviaControllerAccess::template bindStream<&RuviaControllerType::proxy>(
            this
        ),
        RuviaControllerAccess::template makeMiddlewares<>()
    );
    RUVIA_ROUTES_END

  private:
    static std::string percentEncode(std::string_view value);
    static std::string queryString(const ruvia::ContextRequest& request);
    ruvia::Task<void> proxy(ruvia::Context& context);
};

} // namespace service::gb28181
