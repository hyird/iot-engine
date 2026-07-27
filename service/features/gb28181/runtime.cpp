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
            config_.media.zlmBaseUrl.empty() || config_.media.rtpPublicIp.empty()) {
            throw std::runtime_error(
                "GB28181 configuration requires domain, id, public IP, password, ZLM URL and RTP IP");
        }
        if (config_.media.rtpPortRangeStart > config_.media.rtpPortRangeEnd)
            throw std::runtime_error("GB28181 RTP port range is invalid");

        loopThread_ = std::make_unique<trantor::EventLoopThread>("GB28181-SIP");
        loopThread_->run();
        auto* loop = loopThread_->getLoop();
        if (loop == nullptr)
            throw std::runtime_error("GB28181 event loop failed to start");

        devices_ = std::make_unique<DeviceRegistry>();
        streams_ = std::make_unique<StreamRegistry>();
        zlm_ = std::make_unique<ZlmClient>(config_.media, loop);
        sip_ =
            std::make_unique<SipServer>(config_.sip, config_.media, *devices_, *zlm_, loop);
        drogon::sync_wait(sip_->startCoro());
        lastError_.clear();
    } catch (const std::exception& error) {
        lastError_ = error.what();
        started_.store(false);
        stop();
        throw;
    }
}

void Runtime::stop() noexcept {
    if (!started_.exchange(false) && !loopThread_)
        return;
    try {
        if (sip_)
            drogon::sync_wait(sip_->stopCoro());
    } catch (const std::exception& error) {
        lastError_ = error.what();
    }
    sip_.reset();
    zlm_.reset();
    streams_.reset();
    devices_.reset();
    if (loopThread_) {
        if (auto* loop = loopThread_->getLoop())
            loop->quit();
        try {
            loopThread_->wait();
        } catch (...) {
        }
        loopThread_.reset();
    }
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

bool Runtime::queryCatalog(std::string_view deviceId) {
    requireStarted();
    return sip_->queryCatalog(std::string(deviceId));
}

bool Runtime::queryRecords(std::string_view deviceId, std::string_view channelId,
                           std::string_view startTime, std::string_view endTime) {
    requireStarted();
    return sip_->queryRecords(std::string(deviceId), std::string(channelId),
                              std::string(startTime), std::string(endTime));
}

bool Runtime::ptz(std::string_view deviceId, std::string_view channelId,
                  std::string_view action, std::uint8_t speed) {
    requireStarted();
    return sip_->sendPtzControl(std::string(deviceId), std::string(channelId),
                                std::string(action), speed);
}

bool Runtime::ptzPosition(std::string_view deviceId, std::string_view channelId, double pan,
                          double tilt, double zoom) {
    requireStarted();
    return sip_->sendPtzPreciseControl(std::string(deviceId), std::string(channelId), pan, tilt,
                                       zoom);
}

std::future<std::optional<SipServer::PreviewStartResult>>
Runtime::startPreview(std::string deviceId, std::string channelId) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStartResult>>(
        [this, deviceId = std::move(deviceId),
         channelId = std::move(channelId)]() -> drogon::Task<
            std::optional<SipServer::PreviewStartResult>> {
            co_return co_await sip_->startPreviewCoro(deviceId, channelId);
        });
}

std::future<std::optional<SipServer::PreviewStartResult>>
Runtime::startPlayback(std::string deviceId, std::string channelId, std::string startTime,
                       std::string endTime) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStartResult>>(
        [this, deviceId = std::move(deviceId), channelId = std::move(channelId),
         startTime = std::move(startTime),
         endTime = std::move(endTime)]() -> drogon::Task<
            std::optional<SipServer::PreviewStartResult>> {
            co_return co_await sip_->startPlaybackCoro(deviceId, channelId, startTime, endTime);
        });
}

std::future<std::optional<SipServer::PreviewStopResult>>
Runtime::stopPreview(std::string sessionId) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStopResult>>(
        [this, sessionId = std::move(sessionId)]() -> drogon::Task<
            std::optional<SipServer::PreviewStopResult>> {
            co_return co_await sip_->stopPreviewCoro(sessionId);
        });
}

std::future<std::optional<SipServer::PreviewStopResult>>
Runtime::stopPreviewByStream(std::string streamId) {
    requireStarted();
    return launch<std::optional<SipServer::PreviewStopResult>>(
        [this, streamId = std::move(streamId)]() -> drogon::Task<
            std::optional<SipServer::PreviewStopResult>> {
            co_return co_await sip_->stopPreviewByStreamCoro(streamId);
        });
}

std::future<bool> Runtime::forceCloseRtp(std::string streamId) {
    requireStarted();
    return launch<bool>([this, streamId = std::move(streamId)]() -> drogon::Task<bool> {
        co_return co_await sip_->forceCloseRtpServerCoro(streamId);
    });
}

std::vector<StreamStatus> Runtime::streams() const {
    return streams_ ? streams_->listStreams() : std::vector<StreamStatus>{};
}

std::optional<StreamStatus> Runtime::stream(std::string_view id) const {
    return streams_ ? streams_->findStream(std::string(id)) : std::nullopt;
}

void Runtime::streamChanged(std::string app, std::string stream, std::string schema, bool online) {
    if (!streams_)
        return;
    streams_->updateStreamChanged(app, stream, schema, online);
    if (sip_)
        sip_->markStreamOnline(stream, online);
}

void Runtime::streamNoneReader(std::string app, std::string stream, std::string schema) {
    if (streams_)
        streams_->updateNoneReader(std::move(app), stream, std::move(schema));
}

void Runtime::requireStarted() const {
    if (!started_.load() || !sip_)
        throw std::runtime_error(config_.enabled ? "GB28181 runtime is not started"
                                                : "GB28181 is disabled");
}

} // namespace service::gb28181
