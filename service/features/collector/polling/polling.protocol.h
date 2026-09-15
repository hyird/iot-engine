#pragma once

#include <deque>
#include "service/features/collector/collector.protocol.h"
#include "service/features/collector/polling/polling.types.h"
#include "service/features/collector/scheduling/scheduling.protocol.h"

namespace service::collector::polling {

// 单连接内顺序执行请求；协议负责报文和握手，调度不接触网络或数据库。
template <typename Exchange>
class Session final : public ProtocolSession, public CommandCapabilitySession,
                      public DeadlineCapabilitySession {
  public:
    Session(LinkDefinition link, std::string connectionId,
            std::shared_ptr<const RuntimeSnapshot> snapshot,
            std::vector<const DeviceDefinition*> devices)
        : link_(std::move(link)), connectionId_(std::move(connectionId)),
          snapshot_(std::move(snapshot)), candidates_(std::move(devices)) {}

    std::vector<ProtocolAction> connected() override {
        std::vector<ProtocolAction> actions;
        try {
        if (link_.mode == "TCP Client") bind(candidates_, actions);
        else {
            std::vector<const DeviceDefinition*> direct;
            for (const auto* device : candidates_)
                if (device->registrationMode == "OFF") direct.push_back(device);
            if (direct.size() == candidates_.size()) bind(direct, actions);
        }
        advance(actions);
        } catch (const std::exception& error) { close(error.what(), actions); }
        return actions;
    }

    std::vector<ProtocolAction> consume(const ProtocolInput& input) override {
        if (closed_) return {};
        std::vector<ProtocolAction> actions;
        try {
            if (buffer_.size() + input.bytes.size() > 65536)
                throw std::invalid_argument("polling_receive_buffer_overflow");
            buffer_.insert(buffer_.end(), input.bytes.begin(), input.bytes.end());
            if (devices_.empty()) {
                if (!bindRegistration(actions)) return actions;
                advance(actions);
            }
            while (!buffer_.empty()) {
                if (consumeHeartbeat()) continue;
                if (partialHeartbeat()) break;
                if (!pending_) throw std::invalid_argument("polling_unsolicited_response");
                auto& exchange = devices_[pending_->device].exchange;
                exchange.stripPrefix(buffer_);
                if (buffer_.empty()) break;
                const auto length = exchange.frameLength(buffer_);
                if (!length || buffer_.size() < *length) break;
                std::vector<std::uint8_t> frame(buffer_.begin(), buffer_.begin() + *length);
                buffer_.erase(buffer_.begin(), buffer_.begin() + *length);
                // 下一请求尚未发送，剩余完整帧不能用来确认下一次操作。
                while (consumeHeartbeat()) {}
                if (!buffer_.empty())
                    throw std::invalid_argument("polling_unexpected_trailing_response");
                receiveFrame(input, frame, actions);
            }
            advance(actions);
        } catch (const std::exception& error) {
            close(error.what(), actions);
        }
        return actions;
    }

    std::vector<ProtocolAction> disconnected(std::string_view reason) override {
        std::vector<ProtocolAction> actions;
        terminate(reason, actions);
        return actions;
    }

    std::vector<ProtocolAction> execute(ProtocolCommand command) override {
        const auto fail = [&](std::string reason) {
            return std::vector<ProtocolAction>{{.kind = ProtocolActionKind::FailCommand,
                .connectionId = connectionId_, .deviceId = command.deviceId,
                .deviceCode = command.deviceCode, .commandId = command.id, .reason = std::move(reason)}};
        };
        if (closed_) return fail("polling_connection_closed");
        const auto found = std::find_if(devices_.begin(), devices_.end(), [&](const auto& state) {
            return state.device->id == command.deviceId;
        });
        if (found == devices_.end()) return fail("polling_device_offline");
        if (commands_.size() >= 256) return fail("polling_command_queue_full");
        try {
            if (!command.payload.empty() || command.elements.size() != 1)
                return fail("command_invalid: exactly one configured point is required");
            const auto resolved = command::resolve(*found->device, command.elements);
            auto encoded = found->exchange.encode(*resolved.elements[0].definition, resolved.elements[0].value);
            Operation operation{static_cast<std::size_t>(found - devices_.begin()),
                resolved.elements[0].definition, std::move(command), std::move(encoded)};
            commands_.push_back(std::move(operation));
            std::stable_sort(commands_.begin(), commands_.end(), [](const auto& left, const auto& right) {
                return left.command.highPriority && !right.command.highPriority;
            });
        } catch (const std::exception& error) { return fail(error.what()); }
        std::vector<ProtocolAction> actions;
        try { advance(actions); }
        catch (const std::exception& error) { close(error.what(), actions); }
        return actions;
    }

    std::vector<ProtocolAction> deadline(std::uint64_t token) override {
        std::vector<ProtocolAction> actions;
        if (closed_ || token == 0) return actions;
        if (pending_ && token == requestDeadline_) {
            requestDeadline_ = 0;
            close(pending_->phase == Phase::Write ? "polling_write_result_unknown" : "polling_response_timeout", actions);
            return actions;
        }
        for (auto& state : devices_) if (token == state.pollDeadline) {
            state.pollDeadline = 0;
            state.pollIndex = 0;
            state.polling = !state.device->elements.empty();
            try { advance(actions); }
            catch (const std::exception& error) { close(error.what(), actions); }
            break;
        }
        return actions;
    }

  private:
    enum class Phase { Handshake, Read, Write, Readback };
    struct DeviceState {
        const DeviceDefinition* device;
        Exchange exchange;
        bool ready = false;
        bool polling = false;
        std::size_t pollIndex = 0;
        std::uint64_t pollDeadline = 0;
        AcquisitionCycle cycle;
        std::chrono::steady_clock::time_point fastReadUntil{};
    };
    struct Operation {
        std::size_t device = 0;
        const ElementDefinition* element = nullptr;
        ProtocolCommand command;
        std::vector<std::uint8_t> expected;
        Phase phase = Phase::Write;
        std::vector<std::vector<std::uint8_t>> rawFrames;
        std::vector<std::string> rawPacketIds;
    };

    void bind(const std::vector<const DeviceDefinition*>& selected, std::vector<ProtocolAction>& actions) {
        if (selected.empty()) return;
        if (!Exchange::sharedConnection && selected.size() > 1)
            throw std::invalid_argument("polling_connection_requires_one_device");
        for (const auto* device : selected) {
            devices_.push_back({device, Exchange(*device), false, !device->elements.empty()});
            actions.push_back({.kind = ProtocolActionKind::BindDevice, .connectionId = connectionId_,
                .deviceId = device->id, .deviceCode = device->code});
        }
    }

    bool bindRegistration(std::vector<ProtocolAction>& actions) {
        std::vector<const DeviceDefinition*> matched;
        std::size_t length = 0;
        bool partial = false;
        for (const auto* device : candidates_) {
            const auto& registration = device->registrationBytes;
            if (device->registrationMode == "OFF" || registration.empty()) continue;
            const auto prefix = std::min(buffer_.size(), registration.size());
            if (!std::equal(buffer_.begin(), buffer_.begin() + prefix, registration.begin())) continue;
            if (buffer_.size() < registration.size()) { partial = true; continue; }
            if (length != 0 && length != registration.size())
                throw std::invalid_argument("polling_registration_ambiguous");
            length = registration.size();
            matched.push_back(device);
        }
        if (partial) return false;
        if (matched.empty()) throw std::invalid_argument("polling_registration_unknown");
        buffer_.erase(buffer_.begin(), buffer_.begin() + length);
        bind(matched, actions);
        return true;
    }

    bool consumeHeartbeat() {
        for (const auto& state : devices_) {
            const auto& heartbeat = state.device->heartbeatBytes;
            if (state.device->heartbeatMode == "OFF" || heartbeat.empty() || buffer_.size() < heartbeat.size()) continue;
            if (std::equal(heartbeat.begin(), heartbeat.end(), buffer_.begin())) {
                buffer_.erase(buffer_.begin(), buffer_.begin() + heartbeat.size());
                return true;
            }
        }
        return false;
    }

    bool partialHeartbeat() const {
        for (const auto& state : devices_) {
            const auto& heartbeat = state.device->heartbeatBytes;
            if (state.device->heartbeatMode != "OFF" && buffer_.size() < heartbeat.size() &&
                std::equal(buffer_.begin(), buffer_.end(), heartbeat.begin())) return true;
        }
        return false;
    }

    void send(std::vector<std::uint8_t> bytes, std::vector<ProtocolAction>& actions) {
        const auto& operation = *pending_;
        const auto& device = *devices_[operation.device].device;
        actions.push_back({.kind = ProtocolActionKind::Send, .connectionId = connectionId_,
            .deviceId = device.id, .deviceCode = device.code, .commandId = operation.command.id,
            .bytes = std::move(bytes),
            .acquisitionId = operation.command.id.empty() ? devices_[operation.device].cycle.id(connectionId_ + devices_[operation.device].device->id) : operation.command.id});
        requestDeadline_ = nextToken_++;
        actions.push_back({.kind = ProtocolActionKind::ScheduleDeadline, .connectionId = connectionId_,
            .deviceId = device.id, .commandId = operation.command.id, .deadlineToken = requestDeadline_,
            .deadlineAfter = operation.command.id.empty() ? std::chrono::milliseconds(5000) :
                std::clamp(operation.command.timeout, std::chrono::milliseconds(100), std::chrono::milliseconds(60000))});
    }

    void cancelRequest(std::vector<ProtocolAction>& actions) {
        if (!requestDeadline_) return;
        actions.push_back({.kind = ProtocolActionKind::CancelDeadline, .connectionId = connectionId_,
            .deadlineToken = requestDeadline_});
        requestDeadline_ = 0;
    }

    void advance(std::vector<ProtocolAction>& actions) {
        if (closed_ || pending_) return;
        for (std::size_t i = 0; i < devices_.size(); ++i) if (!devices_[i].ready) {
            auto bytes = devices_[i].exchange.handshake();
            if (!bytes.empty()) {
                pending_ = Operation{.device = i, .phase = Phase::Handshake};
                send(std::move(bytes), actions);
                return;
            }
            devices_[i].ready = true;
        }
        if (!commands_.empty()) {
            pending_ = std::move(commands_.front()); commands_.pop_front();
            auto& operation = *pending_;
            auto& state = devices_[operation.device];
            if (state.pollDeadline) {
                actions.push_back({.kind = ProtocolActionKind::CancelDeadline, .connectionId = connectionId_,
                    .deadlineToken = state.pollDeadline});
                state.pollDeadline = 0;
            }
            state.cycle.fail(); state.cycle.finish(actions);
            state.pollIndex = 0;
            state.polling = !state.device->elements.empty();
            send(devices_[operation.device].exchange.write(*operation.element, operation.expected), actions);
            return;
        }
        for (std::size_t count = 0; count < devices_.size(); ++count) {
            const auto i = (roundRobin_ + count) % devices_.size();
            auto& state = devices_[i];
            if (!state.polling) continue;
            if (state.pollIndex >= state.device->elements.size()) {
                state.polling = false;
                state.cycle.finish(actions);
                state.pollDeadline = nextToken_++;
                const auto interval = std::chrono::steady_clock::now() < state.fastReadUntil
                    ? state.device->commandFastReadInterval : state.device->readInterval;
                actions.push_back({.kind = ProtocolActionKind::ScheduleDeadline, .connectionId = connectionId_,
                    .deviceId = state.device->id, .deadlineToken = state.pollDeadline,
                    .deadlineAfter = staggeredPollDelay(state.device->id,
                        std::chrono::seconds(std::clamp<std::int64_t>(interval, 1, 86400)))});
                continue;
            }
            pending_ = Operation{.device = i, .element = &state.device->elements[state.pollIndex], .phase = Phase::Read};
            roundRobin_ = (i + 1) % devices_.size();
            send(state.exchange.read(*pending_->element), actions);
            return;
        }
    }

    void receiveFrame(const ProtocolInput& input, const std::vector<std::uint8_t>& frame,
                      std::vector<ProtocolAction>& actions) {
        auto& operation = *pending_;
        auto& state = devices_[operation.device];
        if (operation.phase == Phase::Handshake) {
            state.exchange.acceptHandshake(frame);
            cancelRequest(actions);
            state.ready = true;
            pending_.reset();
            return;
        }
        auto reply = state.exchange.response(*operation.element, operation.phase == Phase::Write, frame);
        cancelRequest(actions);
        actions.back().responseSuccess = reply.error.empty();
        if (!reply.error.empty()) {
            if (operation.command.id.empty()) { state.cycle.fail(); ++state.pollIndex; }
            else actions.push_back(commandResult(operation, false, reply.error));
            pending_.reset();
            return;
        }
        operation.rawFrames.push_back(frame);
        operation.rawPacketIds.push_back(std::string(input.messageId) + ":frame:" + std::to_string(nextPacket_++));
        if (!reply.nextRequest.empty()) { send(std::move(reply.nextRequest), actions); return; }
        if (operation.phase == Phase::Write) {
            operation.phase = Phase::Readback;
            send(state.exchange.read(*operation.element), actions);
            return;
        }
        auto parsed = parsedAction(input, state, *operation.element, reply.data, frame, operation.command.id);
        parsed.parsed.rawPayloads = std::move(operation.rawFrames);
        parsed.parsed.rawPacketIds = std::move(operation.rawPacketIds);
        if (operation.phase == Phase::Readback) {
            state.fastReadUntil = std::chrono::steady_clock::now() +
                std::chrono::seconds(std::clamp<std::int64_t>(state.device->commandFastReadDuration, 0, 3600));
            actions.push_back(std::move(parsed));
            actions.push_back(commandResult(operation, reply.data == operation.expected, "polling_readback_mismatch"));
        } else {
            state.cycle.observe(std::move(parsed), actions);
            ++state.pollIndex;
        }
        pending_.reset();
    }

    ProtocolAction parsedAction(const ProtocolInput& input, DeviceState& state, const ElementDefinition& element,
        const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& frame, const std::string& commandId) {
        const auto& device = *state.device;
        message::ParsedDeviceMessage message;
        message.causationId = commandId.empty() ? std::string(input.messageId) : commandId;
        message.linkId = link_.id; message.deviceId = device.id; message.modelId = device.modelId;
        message.deviceCode = device.code; message.protocol = device.protocol; message.connectionId = connectionId_;
        message.occurredAtMs = input.receivedAtMs; message.observedAtMs = input.receivedAtMs;
        message.storagePolicy = device.storagePolicy;
        message.onlineWindowMs = std::clamp<std::int64_t>(device.onlineTimeout, 1, 86400) * 1000;
        message.source = "query"; message.rawPayloads = {frame};
        message.rawPacketIds = {std::string(input.messageId) + ":frame:" + std::to_string(nextPacket_++)};
        message.valuesJson = "{\"function_code\":\"POLL\",\"values\":{" + service::utils::jsonQuoted(element.id) +
            ":{\"name\":" + service::utils::jsonQuoted(element.name) + ",\"value\":" + state.exchange.decode(element, data) +
            ",\"unit\":" + service::utils::jsonQuoted(element.unit) + "}}}";
        return {.kind = ProtocolActionKind::PublishParsed, .connectionId = connectionId_,
            .deviceId = device.id, .deviceCode = device.code, .parsed = std::move(message), .responseSuccess = true};
    }

    ProtocolAction commandResult(const Operation& operation, bool success, std::string_view reason) const {
        const auto& device = *devices_[operation.device].device;
        return {.kind = success ? ProtocolActionKind::CompleteCommand : ProtocolActionKind::FailCommand,
            .connectionId = connectionId_, .deviceId = device.id, .deviceCode = device.code,
            .commandId = operation.command.id, .reason = success ? "" : std::string(reason), .responseSuccess = success};
    }

    void terminate(std::string_view reason, std::vector<ProtocolAction>& actions) {
        if (closed_) return;
        closed_ = true;
        cancelRequest(actions);
        if (pending_ && !pending_->command.id.empty()) actions.push_back(commandResult(*pending_, false, reason));
        for (const auto& command : commands_) actions.push_back(commandResult(command, false, reason));
        for (auto& state : devices_) {
            state.cycle.fail(); state.cycle.finish(actions);
            if (state.pollDeadline) actions.push_back({.kind = ProtocolActionKind::CancelDeadline,
                .connectionId = connectionId_, .deadlineToken = state.pollDeadline});
        }
        pending_.reset(); commands_.clear(); buffer_.clear();
    }

    void close(std::string_view reason, std::vector<ProtocolAction>& actions) {
        std::erase_if(actions, [](const auto& action) {
            return action.kind == ProtocolActionKind::Send || action.kind == ProtocolActionKind::ScheduleDeadline;
        });
        terminate(reason, actions);
        actions.push_back({.kind = ProtocolActionKind::Close, .connectionId = connectionId_,
            .reason = std::string(reason), .responseSuccess = false});
    }

    LinkDefinition link_;
    std::string connectionId_;
    std::shared_ptr<const RuntimeSnapshot> snapshot_;
    std::vector<const DeviceDefinition*> candidates_;
    std::vector<DeviceState> devices_;
    std::deque<Operation> commands_;
    std::optional<Operation> pending_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t nextToken_ = 1, requestDeadline_ = 0, nextPacket_ = 0;
    std::size_t roundRobin_ = 0;
    bool closed_ = false;
};

} // namespace service::collector::polling
