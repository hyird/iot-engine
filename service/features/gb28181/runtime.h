#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <drogon/drogon.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/Context.h>
#include <trantor/net/EventLoopThread.h>

#include "config/AppConfig.h"
#include "device/DeviceRegistry.h"
#include "media/StreamRegistry.h"
#include "media/ZlmClient.h"
#include "sip/SipServer.h"

namespace service::gb28181 {

class Runtime final {
  public:
    static Runtime& instance();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void configure(AppConfig config);
    void start();
    void stop() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }
    [[nodiscard]] bool started() const noexcept { return started_.load(); }
    [[nodiscard]] const AppConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

    [[nodiscard]] std::vector<Device> devices() const;
    [[nodiscard]] std::optional<Device> device(std::string_view id) const;
    void mockRegister(std::string deviceId, std::string remoteAddress);
    [[nodiscard]] bool queryCatalog(std::string_view deviceId);
    [[nodiscard]] bool queryRecords(std::string_view deviceId, std::string_view channelId,
                                    std::string_view startTime, std::string_view endTime);
    [[nodiscard]] bool ptz(std::string_view deviceId, std::string_view channelId,
                           std::string_view action, std::uint8_t speed);
    [[nodiscard]] bool ptzPosition(std::string_view deviceId, std::string_view channelId,
                                   double pan, double tilt, double zoom);

    std::future<std::optional<SipServer::PreviewStartResult>>
    startPreview(std::string deviceId, std::string channelId);
    std::future<std::optional<SipServer::PreviewStartResult>>
    startPlayback(std::string deviceId, std::string channelId, std::string startTime,
                  std::string endTime);
    std::future<std::optional<SipServer::PreviewStopResult>> stopPreview(std::string sessionId);
    std::future<std::optional<SipServer::PreviewStopResult>>
    stopPreviewByStream(std::string streamId);
    std::future<bool> forceCloseRtp(std::string streamId);

    [[nodiscard]] std::vector<StreamStatus> streams() const;
    [[nodiscard]] std::optional<StreamStatus> stream(std::string_view id) const;
    void streamChanged(std::string app, std::string stream, std::string schema, bool online);
    void streamNoneReader(std::string app, std::string stream, std::string schema);

    template <typename T>
    static ruvia::Task<T> wait(ruvia::Context& context, std::future<T> future,
                               std::chrono::seconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("GB28181 operation timed out");
            (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(5));
        }
        co_return future.get();
    }

  private:
    Runtime() = default;
    ~Runtime();

    void requireStarted() const;

    template <typename T, typename Factory>
    static std::future<T> launch(Factory factory) {
        return std::async(std::launch::async, [factory = std::move(factory)]() mutable {
            return drogon::sync_wait(factory());
        });
    }

    AppConfig config_;
    std::unique_ptr<trantor::EventLoopThread> loopThread_;
    std::unique_ptr<DeviceRegistry> devices_;
    std::unique_ptr<StreamRegistry> streams_;
    std::unique_ptr<ZlmClient> zlm_;
    std::unique_ptr<SipServer> sip_;
    std::atomic_bool started_{false};
    std::string lastError_;
};

inline Runtime& runtime() { return Runtime::instance(); }

} // namespace service::gb28181
