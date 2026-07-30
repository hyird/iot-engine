#include "runtime.h"

#include <stdexcept>

namespace service::gb28181 {

Runtime &Runtime::instance() {
  static Runtime value;
  return value;
}

Runtime::~Runtime() { stop(); }

ZlmSdk::Ports Runtime::mediaPorts() const noexcept {
  return zlm_ ? zlm_->ports() : ZlmSdk::Ports{};
}

ZlmSdk::Capabilities Runtime::mediaCapabilities() const noexcept {
  if (zlm_)
    return zlm_->capabilities();
  auto capabilities = ZlmSdk::Capabilities{};
  capabilities.tls = config_.media.tlsEnabled;
  capabilities.recording = config_.media.recordingEnabled;
  return capabilities;
}

void Runtime::configure(AppConfig config) {
  if (started_.load())
    throw std::runtime_error("GB28181 runtime is already started");
  config_ = std::move(config);
}

void Runtime::attachProjector(std::shared_ptr<Projector> projector,
                              Projector::Snapshot snapshot) {
  if (started_.load())
    throw std::runtime_error("GB28181 runtime is already started");
  projector_ = std::move(projector);
  snapshot_ = std::move(snapshot);
}

void Runtime::start() {
  if (!config_.enabled || started_.exchange(true))
    return;
  try {
    if (config_.sip.domain.empty() || config_.sip.id.empty() ||
        config_.sip.publicIp.empty() || config_.sip.password.empty() ||
        config_.media.rtpPublicIp.empty() ||
        config_.media.playTokenSecret.size() < 16) {
      throw std::runtime_error(
          "GB28181 configuration requires domain, id, public IP, password, RTP "
          "IP "
          "and a media token secret of at least 16 characters");
    }
    const auto randomRtpPort = config_.media.rtpPortRangeStart == 0 &&
                               config_.media.rtpPortRangeEnd == 0;
    if (!randomRtpPort &&
        (config_.media.rtpPortRangeStart == 0 ||
         config_.media.rtpPortRangeStart > config_.media.rtpPortRangeEnd))
      throw std::runtime_error("GB28181 RTP port range is invalid");
    if (config_.media.workerThreads <= 0)
      throw std::runtime_error("ZLM worker thread count must be positive");
    if (config_.media.logLevel < 0 || config_.media.logLevel > 4)
      throw std::runtime_error("ZLM log level must be between 0 and 4");
    if (config_.sip.deviceTimezoneOffsetMinutes < -24 * 60 ||
        config_.sip.deviceTimezoneOffsetMinutes > 24 * 60)
      throw std::runtime_error("GB28181 device timezone offset is invalid");

    loopPool_ = std::make_unique<ruvia::EventLoopPool>(
        ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1024});
    loopPool_->start();
    sipLoop_ = loopPool_->loop(0);
    if (!sipLoop_.valid())
      throw std::runtime_error("GB28181 event loop failed to start");

    devices_ = std::make_unique<DeviceRegistry>(
        [projector = projector_](const Device &device,
                                 DeviceRegistry::Change change) {
          if (projector)
            projector->project(device, change);
        });
    devices_->replace(std::move(snapshot_.devices));
    streams_ = std::make_unique<StreamRegistry>(
        [projector = projector_](const StreamStatus &stream) {
          if (projector)
            projector->project(stream);
        });
    streams_->replace(std::move(snapshot_.streams));
    ZlmSdk::Callbacks callbacks;
    callbacks.onStreamChanged = [this](std::string app, std::string stream,
                                       std::string schema, bool online,
                                       int readerCount) {
      streamChanged(std::move(app), std::move(stream), std::move(schema),
                    online, readerCount);
    };
    callbacks.onStreamNoneReader = [this](std::string app, std::string stream,
                                          std::string schema) {
      streamNoneReader(std::move(app), std::move(stream), std::move(schema));
    };
    callbacks.onRtpDetached = [this](std::string stream) {
      scheduleStreamClose(std::move(stream));
    };
    zlm_ = std::make_unique<ZlmSdk>(config_.media, std::move(callbacks));
    zlm_->start();
    sip_ = std::make_unique<SipServer>(config_.sip, config_.media, *devices_,
                                       *zlm_, sipLoop_);
    sip_->start();
    lastError_.clear();
  } catch (const std::exception &error) {
    lastError_ = error.what();
    started_.store(false);
    stop();
    throw;
  }
}

void Runtime::stop() noexcept {
  if (!started_.exchange(false) && !loopPool_)
    return;
  try {
    if (sip_)
      sip_->stop();
    if (zlm_)
      zlm_->stop();
  } catch (const std::exception &error) {
    lastError_ = error.what();
  }
  if (loopPool_) {
    loopPool_->stop();
    try {
      loopPool_->join();
    } catch (const std::exception &error) {
      lastError_ = error.what();
    }
  }
  sip_.reset();
  zlm_.reset();
  streams_.reset();
  devices_.reset();
  sipLoop_ = {};
  loopPool_.reset();
}

ruvia::Task<std::vector<Device>> Runtime::devices(ruvia::Context &context) {
  co_return co_await invoke<std::vector<Device>>(
      context, [this] { return devices_->listDevices(); },
      std::chrono::seconds(2));
}

ruvia::Task<std::optional<Device>> Runtime::device(ruvia::Context &context,
                                                   std::string id) {
  co_return co_await invoke<std::optional<Device>>(
      context, [this, id = std::move(id)] { return devices_->findDevice(id); },
      std::chrono::seconds(2));
}

ruvia::Task<bool> Runtime::mapDevice(ruvia::Context &context, std::string id,
                                     std::string mappedDeviceId) {
  co_return co_await invoke<bool>(
      context,
      [this, id = std::move(id), mappedDeviceId = std::move(mappedDeviceId)] {
        return devices_->updateMapping(id, mappedDeviceId);
      });
}

ruvia::Task<bool> Runtime::queryCatalog(ruvia::Context &context,
                                        std::string deviceId) {
  co_return co_await invoke<bool>(context,
                                  [this, deviceId = std::move(deviceId)] {
                                    return sip_->queryCatalog(deviceId);
                                  });
}

ruvia::Task<bool> Runtime::queryRecords(ruvia::Context &context,
                                        std::string deviceId,
                                        std::string channelId,
                                        std::string startTime,
                                        std::string endTime) {
  co_return co_await invoke<bool>(
      context,
      [this, deviceId = std::move(deviceId), channelId = std::move(channelId),
       startTime = std::move(startTime), endTime = std::move(endTime)] {
        return sip_->queryRecords(deviceId, channelId, startTime, endTime);
      });
}

ruvia::Task<bool> Runtime::ptz(ruvia::Context &context, std::string deviceId,
                               std::string channelId, std::string action,
                               std::uint8_t speed) {
  co_return co_await invoke<bool>(context, [this,
                                            deviceId = std::move(deviceId),
                                            channelId = std::move(channelId),
                                            action = std::move(action), speed] {
    return sip_->sendPtzControl(deviceId, channelId, action, speed);
  });
}

ruvia::Task<bool> Runtime::ptzPosition(ruvia::Context &context,
                                       std::string deviceId,
                                       std::string channelId, double pan,
                                       double tilt, double zoom) {
  co_return co_await invoke<bool>(context, [this,
                                            deviceId = std::move(deviceId),
                                            channelId = std::move(channelId),
                                            pan, tilt, zoom] {
    return sip_->sendPtzPreciseControl(deviceId, channelId, pan, tilt, zoom);
  });
}

ruvia::Task<std::optional<SipServer::PreviewStartResult>>
Runtime::startPreview(ruvia::Context &context, std::string deviceId,
                      std::string channelId) {
  co_return co_await invoke<std::optional<SipServer::PreviewStartResult>>(
      context,
      [this, deviceId = std::move(deviceId),
       channelId = std::move(channelId)]() {
        return sip_->startPreview(deviceId, channelId);
      },
      std::chrono::seconds(15),
      [this](const std::optional<SipServer::PreviewStartResult> &value) {
        if (value)
          (void)sip_->stopPreview(value->sessionId);
      });
}

ruvia::Task<std::optional<SipServer::PreviewStartResult>>
Runtime::startPlayback(ruvia::Context &context, std::string deviceId,
                       std::string channelId, std::string startTime,
                       std::string endTime) {
  co_return co_await invoke<std::optional<SipServer::PreviewStartResult>>(
      context,
      [this, deviceId = std::move(deviceId), channelId = std::move(channelId),
       startTime = std::move(startTime), endTime = std::move(endTime)]() {
        return sip_->startPlayback(deviceId, channelId, startTime, endTime);
      },
      std::chrono::seconds(15),
      [this](const std::optional<SipServer::PreviewStartResult> &value) {
        if (value)
          (void)sip_->stopPreview(value->sessionId);
      });
}

ruvia::Task<std::optional<SipServer::PreviewStopResult>>
Runtime::stopPreview(ruvia::Context &context, std::string sessionId) {
  co_return co_await invoke<std::optional<SipServer::PreviewStopResult>>(
      context, [this, sessionId = std::move(sessionId)]() {
        return sip_->stopPreview(sessionId);
      });
}

ruvia::Task<std::optional<SipServer::PreviewStopResult>>
Runtime::stopPreviewByStream(ruvia::Context &context, std::string streamId) {
  co_return co_await invoke<std::optional<SipServer::PreviewStopResult>>(
      context, [this, streamId = std::move(streamId)]() {
        return sip_->stopPreviewByStream(streamId);
      });
}

ruvia::Task<std::vector<StreamStatus>>
Runtime::streams(ruvia::Context &context) {
  co_return co_await invoke<std::vector<StreamStatus>>(
      context, [this] { return streams_->listStreams(); },
      std::chrono::seconds(2));
}

ruvia::Task<std::optional<StreamStatus>>
Runtime::stream(ruvia::Context &context, std::string id) {
  co_return co_await invoke<std::optional<StreamStatus>>(
      context, [this, id = std::move(id)] { return streams_->findStream(id); },
      std::chrono::seconds(2));
}

ruvia::Task<bool> Runtime::startRecording(ruvia::Context &context,
                                          std::string streamId) {
  co_return co_await invoke<bool>(context,
                                  [this, streamId = std::move(streamId)] {
                                    return zlm_->startMp4Recording(streamId);
                                  });
}

ruvia::Task<bool> Runtime::stopRecording(ruvia::Context &context,
                                         std::string streamId) {
  co_return co_await invoke<bool>(context,
                                  [this, streamId = std::move(streamId)] {
                                    return zlm_->stopMp4Recording(streamId);
                                  });
}

ruvia::Task<bool> Runtime::recording(ruvia::Context &context,
                                     std::string streamId) {
  co_return co_await invoke<bool>(
      context,
      [this, streamId = std::move(streamId)] {
        return zlm_->isMp4Recording(streamId);
      },
      std::chrono::seconds(2));
}

void Runtime::streamChanged(std::string app, std::string stream,
                            std::string schema, bool online, int readerCount) {
  if (!started_.load() || !sipLoop_.valid())
    return;
  (void)sipLoop_.post([this, app = std::move(app), stream = std::move(stream),
                       schema = std::move(schema), online, readerCount] {
    if (!started_.load())
      return;
    if (streams_)
      streams_->updateStreamChanged(app, stream, schema, online, readerCount);
    if (sip_)
      sip_->markStreamOnline(stream, online);
  });
}

void Runtime::streamNoneReader(std::string app, std::string stream,
                               std::string schema) {
  if (!started_.load() || !sipLoop_.valid())
    return;
  (void)sipLoop_.post([this, app = std::move(app), stream = std::move(stream),
                       schema = std::move(schema)] {
    if (!started_.load())
      return;
    if (streams_)
      streams_->updateNoneReader(app, stream, schema);
    if (!sip_ || !zlm_)
      return;
    const auto stopped = sip_->stopPreviewByStream(stream);
    if (!stopped)
      (void)zlm_->closeRtpServer(stream);
  });
}

void Runtime::scheduleStreamClose(std::string stream) {
  if (!started_.load() || stream.empty() || !sipLoop_.valid())
    return;
  (void)sipLoop_.post([this, stream = std::move(stream)] {
    if (!started_.load() || !sip_ || !zlm_)
      return;
    const auto stopped = sip_->stopPreviewByStream(stream);
    if (!stopped)
      (void)zlm_->closeRtpServer(stream);
  });
}

void Runtime::requireStarted() const {
  if (!started_.load() || !sip_)
    throw std::runtime_error(config_.enabled ? "GB28181 runtime is not started"
                                             : "GB28181 is disabled");
}

} // namespace service::gb28181
