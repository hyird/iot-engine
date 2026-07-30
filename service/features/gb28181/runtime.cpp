#include "runtime.h"

#include <stdexcept>

namespace service::gb28181 {

Runtime& Runtime::instance() {
    static Runtime value;
    return value;
}

Runtime::~Runtime() { stop(); }

void Runtime::configure(AppConfig config) {
    if (started_.load())
        throw std::runtime_error("GB28181 runtime is already started");
    config_ = std::move(config);
}

void Runtime::start() {
    if (!config_.enabled || started_.exchange(true))
        return;
    try {
        if (config_.sip.domain.empty() || config_.sip.id.empty() ||
            config_.sip.publicIp.empty() || config_.sip.password.empty() ||
            config_.media.rtpPublicIp.empty()) {
            throw std::runtime_error(
                "GB28181 configuration requires domain, id, public IP, password and RTP IP");
        }
        const auto randomRtpPort = config_.media.rtpPortRangeStart == 0 &&
                                   config_.media.rtpPortRangeEnd == 0;
        if (!randomRtpPort &&
            (config_.media.rtpPortRangeStart == 0 ||
             config_.media.rtpPortRangeStart > config_.media.rtpPortRangeEnd))
            throw std::runtime_error("GB28181 RTP port range is invalid");
        if (config_.media.workerThreads < 0)
            throw std::runtime_error("ZLM worker thread count cannot be negative");
        if (config_.media.logLevel < 0 || config_.media.logLevel > 4)
            throw std::runtime_error("ZLM log level must be between 0 and 4");

        loopPool_ = std::make_unique<ruvia::EventLoopPool>(
            ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 1024});
        loopPool_->start();
        sipLoop_ = loopPool_->loop(0);
        if (!sipLoop_.valid())
            throw std::runtime_error("GB28181 event loop failed to start");

        devices_ = std::make_unique<DeviceRegistry>();
        streams_ = std::make_unique<StreamRegistry>();
        ZlmSdk::Callbacks callbacks;
        callbacks.onStreamChanged =
            [this](std::string app, std::string stream, std::string schema, bool online,
                   int readerCount) {
                streamChanged(std::move(app), std::move(stream), std::move(schema), online,
                              readerCount);
            };
        callbacks.onStreamNoneReader =
            [this](std::string app, std::string stream, std::string schema) {
                streamNoneReader(std::move(app), std::move(stream), std::move(schema));
            };
        callbacks.onRtpDetached =
            [this](std::string stream) { scheduleStreamClose(std::move(stream)); };
        zlm_ = std::make_unique<ZlmSdk>(config_.media, std::move(callbacks));
        zlm_->start();
        sip_ = std::make_unique<SipServer>(config_.sip, config_.media, *devices_, *zlm_,
                                           sipLoop_);
        sip_->start();
        lastError_.clear();
    } catch (const std::exception& error) {
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
        if (zlm_)
            zlm_->stop();
        if (sip_)
            sip_->stop();
    } catch (const std::exception& error) {
        lastError_ = error.what();
    }
    if (loopPool_) {
        loopPool_->stop();
        try {
            loopPool_->join();
        } catch (const std::exception& error) {
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

std::vector<Device> Runtime::devices() const {
    return devices_ ? devices_->listDevices() : std::vector<Device>{};
}

std::optional<Device> Runtime::device(std::string_view id) const {
    return devices_ ? devices_->findDevice(std::string(id)) : std::nullopt;
}

void Runtime::mockRegister(std::string deviceId, std::string remoteAddress) {
    requireStarted();
    devices_->upsertRegistration(deviceId, remoteAddress, "mock");
}

std::future<bool> Runtime::queryCatalog(std::string deviceId) {
    requireStarted();
    return launch<bool>([this, deviceId = std::move(deviceId)] {
        return sip_->queryCatalog(deviceId);
    });
}

std::future<bool> Runtime::queryRecords(std::string deviceId, std::string channelId,
                                        std::string startTime, std::string endTime) {
    requireStarted();
    return launch<bool>(
        [this, deviceId = std::move(deviceId), channelId = std::move(channelId),
         startTime = std::move(startTime), endTime = std::move(endTime)] {
            return sip_->queryRecords(deviceId, channelId, startTime, endTime);
        });
}

std::future<bool> Runtime::ptz(std::string deviceId, std::string channelId,
                               std::string action, std::uint8_t speed) {
    requireStarted();
    return launch<bool>(
        [this, deviceId = std::move(deviceId), channelId = std::move(channelId),
         action = std::move(action), speed] {
            return sip_->sendPtzControl(deviceId, channelId, action, speed);
        });
}

std::future<bool> Runtime::ptzPosition(std::string deviceId, std::string channelId,
                                       double pan, double tilt, double zoom) {
    requireStarted();
    return launch<bool>(
        [this, deviceId = std::move(deviceId), channelId = std::move(channelId), pan,
         tilt, zoom] {
            return sip_->sendPtzPreciseControl(deviceId, channelId, pan, tilt, zoom);
        });
}

std::future<std::optional<SipServer::PreviewStartResult>>
Runtime::startPreview(std::string deviceId, std::string channelId) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStartResult>>(
        [this, deviceId = std::move(deviceId),
         channelId = std::move(channelId)]() {
            return sip_->startPreview(deviceId, channelId);
        });
}

std::future<std::optional<SipServer::PreviewStartResult>>
Runtime::startPlayback(std::string deviceId, std::string channelId, std::string startTime,
                       std::string endTime) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStartResult>>(
        [this, deviceId = std::move(deviceId), channelId = std::move(channelId),
         startTime = std::move(startTime),
         endTime = std::move(endTime)]() {
            return sip_->startPlayback(deviceId, channelId, startTime, endTime);
        });
}

std::future<std::optional<SipServer::PreviewStopResult>>
Runtime::stopPreview(std::string sessionId) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStopResult>>(
        [this, sessionId = std::move(sessionId)]() {
            return sip_->stopPreview(sessionId);
        });
}

std::future<std::optional<SipServer::PreviewStopResult>>
Runtime::stopPreviewByStream(std::string streamId) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStopResult>>(
        [this, streamId = std::move(streamId)]() {
            return sip_->stopPreviewByStream(streamId);
        });
}

std::vector<StreamStatus> Runtime::streams() const {
    return streams_ ? streams_->listStreams() : std::vector<StreamStatus>{};
}

std::optional<StreamStatus> Runtime::stream(std::string_view id) const {
    return streams_ ? streams_->findStream(std::string(id)) : std::nullopt;
}

void Runtime::streamChanged(std::string app, std::string stream, std::string schema, bool online,
                            int readerCount) {
    if (!started_.load() || !sipLoop_.valid())
        return;
    (void)sipLoop_.post(
        [this, app = std::move(app), stream = std::move(stream),
         schema = std::move(schema), online, readerCount] {
            if (!started_.load())
                return;
            if (streams_)
                streams_->updateStreamChanged(app, stream, schema, online, readerCount);
            if (sip_)
                sip_->markStreamOnline(stream, online);
        });
}

void Runtime::streamNoneReader(std::string app, std::string stream, std::string schema) {
    if (!started_.load() || !sipLoop_.valid())
        return;
    (void)sipLoop_.post(
        [this, app = std::move(app), stream = std::move(stream),
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
