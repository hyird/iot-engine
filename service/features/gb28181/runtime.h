#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "config/AppConfig.h"
#include "device/DeviceRegistry.h"
#include "media/MediaProxy.h"
#include "media/StreamRegistry.h"
#include "media/ZlmSdk.h"
#include "projector.h"
#include "sip/SipServer.h"

namespace service::gb28181 {

class Runtime final {
public:
  static Runtime &instance();

  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  void configure(AppConfig config);
  void attachProjector(std::shared_ptr<Projector> projector,
                       Projector::Snapshot snapshot);
  void start();
  void stop() noexcept;

  [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }
  [[nodiscard]] bool started() const noexcept { return started_.load(); }
  [[nodiscard]] const AppConfig &config() const noexcept { return config_; }
  [[nodiscard]] const std::string &lastError() const noexcept {
    return lastError_;
  }
  [[nodiscard]] ZlmSdk::Ports mediaPorts() const noexcept;
  [[nodiscard]] ZlmSdk::Capabilities mediaCapabilities() const noexcept;
  [[nodiscard]] std::shared_ptr<MediaProxySession>
  openMediaProxy(MediaProxyRequest request) const;

  ruvia::Task<std::vector<Device>> devices(ruvia::Context &context);
  ruvia::Task<std::optional<Device>> device(ruvia::Context &context,
                                            std::string id);
  ruvia::Task<bool> mapDevice(ruvia::Context &context, std::string id,
                              std::string mappedDeviceId);
  ruvia::Task<bool> renameDevice(ruvia::Context &context, std::string id,
                                 std::string customName);
  ruvia::Task<bool> renameChannel(ruvia::Context &context, std::string deviceId,
                                  std::string channelId,
                                  std::string customName);
  ruvia::Task<bool> queryCatalog(ruvia::Context &context, std::string deviceId);
  ruvia::Task<bool> queryRecords(ruvia::Context &context, std::string deviceId,
                                 std::string channelId, std::string startTime,
                                 std::string endTime);
  ruvia::Task<bool> ptz(ruvia::Context &context, std::string deviceId,
                        std::string channelId, std::string action,
                        std::uint8_t speed);
  ruvia::Task<bool> ptzPosition(ruvia::Context &context, std::string deviceId,
                                std::string channelId, double pan, double tilt,
                                double zoom);

  ruvia::Task<std::optional<SipServer::PreviewStartResult>>
  startPreview(ruvia::Context &context, std::string deviceId,
               std::string channelId);
  ruvia::Task<std::optional<SipServer::PreviewStartResult>>
  startPlayback(ruvia::Context &context, std::string deviceId,
                std::string channelId, std::string startTime,
                std::string endTime);
  ruvia::Task<std::optional<SipServer::PreviewStopResult>>
  stopPreview(ruvia::Context &context, std::string sessionId);
  ruvia::Task<bool> renewPreview(ruvia::Context &context,
                                 std::string sessionId);
  ruvia::Task<std::optional<SipServer::PreviewStopResult>>
  stopPreviewByStream(ruvia::Context &context, std::string streamId);

  ruvia::Task<std::vector<StreamStatus>> streams(ruvia::Context &context);
  ruvia::Task<std::optional<StreamStatus>> stream(ruvia::Context &context,
                                                  std::string id);
  ruvia::Task<bool> startRecording(ruvia::Context &context,
                                   std::string streamId);
  ruvia::Task<bool> stopRecording(ruvia::Context &context,
                                  std::string streamId);
  ruvia::Task<bool> recording(ruvia::Context &context, std::string streamId);
  void streamChanged(std::string app, std::string stream, std::string schema,
                     bool online, int readerCount);

private:
  template <typename T> struct DispatchResult {
    std::optional<T> value;
    std::exception_ptr error;
  };

  template <typename T> struct NoRollback {
    void operator()(const T &) const noexcept {}
  };

  Runtime() = default;
  ~Runtime();

  void requireStarted() const;
  void scheduleStreamClose(std::string stream);

  template <typename T, typename Factory, typename Rollback = NoRollback<T>>
  ruvia::Task<T> invoke(ruvia::Context &context, Factory factory,
                        std::chrono::seconds timeout = std::chrono::seconds(10),
                        Rollback rollback = {}) {
    requireStarted();
    auto [completion, receiver] =
        ruvia::makeOneShot<DispatchResult<T>>(context.worker());
    auto sharedCompletion =
        std::make_shared<ruvia::OneShotCompletion<DispatchResult<T>>>(
            std::move(completion));
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    const auto posted = sipLoop_.post(
        [sharedCompletion, cancelled, factory = std::move(factory),
         rollback = std::move(rollback)]() mutable {
          if (cancelled->load())
            return;
          DispatchResult<T> outcome;
          try {
            outcome.value.emplace(factory());
          } catch (...) {
            outcome.error = std::current_exception();
          }
          auto completed = sharedCompletion->complete(std::move(outcome));
          if (!completed.accepted()) {
            auto rejected = std::move(completed).takeRejected();
            if (rejected && rejected->value)
              rollback(*rejected->value);
          }
        });
    if (!posted.accepted())
      throw std::runtime_error("GB28181 SIP worker rejected operation");

    auto waited = co_await receiver.waitFor(timeout);
    if (!waited.hasValue()) {
      cancelled->store(true);
      if (waited.status() == ruvia::WorkerWaitStatus::kTimedOut)
        throw std::runtime_error("GB28181 operation timed out");
      if (waited.status() == ruvia::WorkerWaitStatus::kWorkerStopping)
        throw std::runtime_error("GB28181 service worker is stopping");
      throw std::runtime_error("GB28181 operation was cancelled");
    }
    auto outcome = std::move(waited).takeValue();
    if (outcome.error)
      std::rethrow_exception(outcome.error);
    if (!outcome.value)
      throw std::runtime_error("GB28181 operation completed without a value");
    co_return std::move(*outcome.value);
  }

  AppConfig config_;
  std::unique_ptr<ruvia::EventLoopPool> loopPool_;
  ruvia::EventLoop sipLoop_;
  ruvia::EventLoop mediaProxyLoop_;
  std::unique_ptr<DeviceRegistry> devices_;
  std::unique_ptr<StreamRegistry> streams_;
  std::unique_ptr<ZlmSdk> zlm_;
  std::shared_ptr<SipServer> sip_;
  std::shared_ptr<Projector> projector_;
  Projector::Snapshot snapshot_;
  std::atomic_bool started_{false};
  std::string lastError_;
};

inline Runtime &runtime() { return Runtime::instance(); }

} // namespace service::gb28181
