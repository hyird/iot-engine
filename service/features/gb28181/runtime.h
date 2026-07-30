#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/Context.h>

#include "config/AppConfig.h"
#include "device/DeviceRegistry.h"
#include "media/StreamRegistry.h"
#include "media/ZlmSdk.h"
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
    std::future<bool> queryCatalog(std::string deviceId);
    std::future<bool> queryRecords(std::string deviceId, std::string channelId,
                                   std::string startTime, std::string endTime);
    std::future<bool> ptz(std::string deviceId, std::string channelId,
                          std::string action, std::uint8_t speed);
    std::future<bool> ptzPosition(std::string deviceId, std::string channelId, double pan,
                                  double tilt, double zoom);

    std::future<std::optional<SipServer::PreviewStartResult>>
    startPreview(std::string deviceId, std::string channelId);
    std::future<std::optional<SipServer::PreviewStartResult>>
    startPlayback(std::string deviceId, std::string channelId, std::string startTime,
                  std::string endTime);
    std::future<std::optional<SipServer::PreviewStopResult>> stopPreview(std::string sessionId);
    std::future<std::optional<SipServer::PreviewStopResult>>
    stopPreviewByStream(std::string streamId);

    [[nodiscard]] std::vector<StreamStatus> streams() const;
    [[nodiscard]] std::optional<StreamStatus> stream(std::string_view id) const;
    void streamChanged(std::string app, std::string stream, std::string schema, bool online,
                       int readerCount);
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
    void scheduleStreamClose(std::string stream);

    template <typename T, typename Factory> std::future<T> launch(Factory factory) {
        auto completion = std::make_shared<std::promise<T>>();
        auto future = completion->get_future();
        const auto posted = sipLoop_.post(
            [completion, factory = std::move(factory)]() mutable {
                try {
                    completion->set_value(factory());
                } catch (...) {
                    completion->set_exception(std::current_exception());
                }
            });
        if (!posted.accepted()) {
            completion->set_exception(std::make_exception_ptr(
                std::runtime_error("GB28181 SIP worker rejected operation")));
        }
        return future;
    }

    AppConfig config_;
    std::unique_ptr<ruvia::EventLoopPool> loopPool_;
    ruvia::EventLoop sipLoop_;
    std::unique_ptr<DeviceRegistry> devices_;
    std::unique_ptr<StreamRegistry> streams_;
    std::unique_ptr<ZlmSdk> zlm_;
    std::unique_ptr<SipServer> sip_;
    std::atomic_bool started_{false};
    std::string lastError_;
};

inline Runtime& runtime() { return Runtime::instance(); }

} // namespace service::gb28181
