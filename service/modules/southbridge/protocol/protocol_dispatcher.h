#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/protocol/dtu_registry.h"
#include "service/modules/southbridge/protocol/protocol_registry.h"
#include "service/modules/southbridge/protocol/session_manager.h"
#include "service/modules/southbridge/queue/redis_stream.h"

namespace service::southbridge {

struct ResolvedIngress {
    DtuDefinition dtu;
    bridge::IngressPacket packet;
};

class ProtocolDispatcher {
  public:
    ProtocolDispatcher(DtuRegistry& dtus, SessionRegistry& sessions, ProtocolRegistry& protocols,
                       bridge::RedisStreamProducer& producer)
        : dtus_(dtus), sessions_(sessions), protocols_(protocols), producer_(producer) {}

    void setDisplacedConnectionHandler(std::function<void(std::string_view)> handler) {
        displacedConnectionHandler_ = std::move(handler);
    }

    void onConnected(std::string connectionId, std::string linkId, std::string remoteAddress,
                     std::string_view targetId = {}) {
        sessions_.connected(connectionId, linkId, remoteAddress);
        producer_.updateSession(
            connectionId, {{"connection_id", connectionId},
                           {"link_id", linkId},
                           {"remote_address", remoteAddress},
                           {"state", "connected"},
                           {"connected_at_ms", std::to_string(bridge::utcNowMilliseconds())},
                           {"last_seen_at_ms", std::to_string(bridge::utcNowMilliseconds())}});
        if (targetId.empty())
            return;
        const auto dtu = dtus_.findClientTarget(linkId, targetId);
        const auto session = sessions_.find(connectionId);
        if (dtu && session)
            bind(*session, *dtu);
    }

    void onDisconnected(std::string_view connectionId) {
        {
            std::lock_guard lock(bufferMutex_);
            receiveBuffers_.erase(std::string(connectionId));
        }
        const auto session = sessions_.disconnected(connectionId);
        producer_.removeSession(connectionId);
    }

    [[nodiscard]] std::vector<ResolvedIngress> process(bridge::IngressPacket packet) {
        std::vector<ResolvedIngress> resolved;
        sessions_.touch(packet.connectionId);
        auto session = sessions_.find(packet.connectionId);
        if (!session) {
            deadLetter(packet, "session_not_found");
            return resolved;
        }
        updateSessionState(*session);

        const auto linkDtus = dtus_.byLink(packet.linkId);
        if (session->dtuKey.empty() && isHeartbeat(packet.payload, linkDtus))
            return resolved;

        std::optional<DtuDefinition> dtu;
        const auto registration = dtus_.matchRegistration(packet.linkId, packet.payload);
        if (registration.kind == RegistrationMatchKind::Conflict) {
            deadLetter(packet, "dtu_registration_conflict");
            return resolved;
        }
        if (registration.kind != RegistrationMatchKind::None) {
            if (!session->dtuKey.empty() && session->dtuKey != registration.dtu.key) {
                deadLetter(packet, "dtu_registration_conflict");
                return resolved;
            }
            dtu = registration.dtu;
            bind(*session, *dtu);
            packet.payload = registration.payload;
            if (registration.kind == RegistrationMatchKind::StandaloneFrame)
                return resolved;
        } else if (!session->dtuKey.empty()) {
            dtu = dtus_.find(session->dtuKey);
        } else {
            const auto candidates = dtus_.byLink(packet.linkId);
            for (const auto* adapter : protocols_.all()) {
                const auto key = adapter->identify(packet, candidates);
                if (!key)
                    continue;
                if (dtu && dtu->key != *key) {
                    deadLetter(packet, "ambiguous_dtu_broadcast");
                    return resolved;
                }
                dtu = dtus_.find(*key);
            }
            if (dtu)
                bind(*session, *dtu);
        }

        if (!dtu) {
            deadLetter(packet, "dtu_registration_unmatched");
            return resolved;
        }
        const auto* adapter = protocols_.find(dtu->protocol);
        if (!adapter) {
            deadLetter(packet, "protocol_adapter_not_registered");
            return resolved;
        }
        for (auto& payload : splitFrames(packet.connectionId, packet.payload, *dtu)) {
            bridge::IngressPacket frame = packet;
            frame.payload = std::move(payload);
            if (isHeartbeat(frame.payload, {*dtu}))
                continue;
            const auto messages = adapter->parse(frame, *dtu);
            if (messages.empty()) {
                deadLetter(frame, "device_route_unmatched");
                continue;
            }
            for (const auto& message : messages)
                producer_.publish(bridge::kParsedStream, bridge::parsedFields(message));
            resolved.push_back({*dtu, std::move(frame)});
        }
        return resolved;
    }

  private:
    std::vector<std::vector<std::uint8_t>> splitFrames(std::string_view connectionId,
                                                       const std::vector<std::uint8_t>& bytes,
                                                       const DtuDefinition& dtu) {
        if (dtu.protocol != "Modbus")
            return bytes.empty() ? std::vector<std::vector<std::uint8_t>>{}
                                 : std::vector<std::vector<std::uint8_t>>{bytes};
        std::vector<std::vector<std::uint8_t>> frames;
        std::lock_guard lock(bufferMutex_);
        auto& buffer = receiveBuffers_[std::string(connectionId)];
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
        if (buffer.size() > kMaximumReceiveBuffer) {
            buffer.clear();
            throw std::runtime_error("Modbus receive buffer exceeded its limit");
        }
        while (!buffer.empty()) {
            const auto heartbeat =
                std::find_if(dtu.devices.begin(), dtu.devices.end(), [&](const auto& device) {
                    return !device.heartbeatBytes.empty() &&
                           buffer.size() >= device.heartbeatBytes.size() &&
                           std::equal(device.heartbeatBytes.begin(), device.heartbeatBytes.end(),
                                      buffer.begin());
                });
            if (heartbeat != dtu.devices.end()) {
                const auto size = heartbeat->heartbeatBytes.size();
                frames.emplace_back(buffer.begin(),
                                    buffer.begin() + static_cast<std::ptrdiff_t>(size));
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
                continue;
            }

            std::size_t frameLength = 0;
            if (buffer.size() >= 8 && buffer[2] == 0 && buffer[3] == 0) {
                const auto payloadLength = static_cast<std::size_t>(
                    (static_cast<std::uint16_t>(buffer[4]) << 8U) | buffer[5]);
                if (payloadLength < 2 || payloadLength > 254) {
                    buffer.erase(buffer.begin());
                    continue;
                }
                frameLength = 6 + payloadLength;
            } else {
                if (buffer.size() < 2)
                    break;
                const auto functionCode = buffer[1];
                if ((functionCode & 0x80U) != 0) {
                    frameLength = 5;
                } else if (functionCode >= 1 && functionCode <= 4) {
                    if (buffer.size() < 3)
                        break;
                    frameLength = static_cast<std::size_t>(buffer[2]) + 5;
                } else if (functionCode == 5 || functionCode == 6 || functionCode == 15 ||
                           functionCode == 16) {
                    frameLength = 8;
                } else {
                    buffer.erase(buffer.begin());
                    continue;
                }
            }
            if (buffer.size() < frameLength)
                break;
            frames.emplace_back(buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(frameLength));
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frameLength));
        }
        if (buffer.empty())
            receiveBuffers_.erase(std::string(connectionId));
        return frames;
    }

    static bool isHeartbeat(const std::vector<std::uint8_t>& bytes,
                            const std::vector<DtuDefinition>& definitions) {
        return std::any_of(definitions.begin(), definitions.end(), [&](const auto& dtu) {
            return std::any_of(dtu.devices.begin(), dtu.devices.end(), [&](const auto& device) {
                return !device.heartbeatBytes.empty() && bytes == device.heartbeatBytes;
            });
        });
    }

    void updateSessionState(const DtuSession& session) {
        const auto dtu =
            session.dtuKey.empty() ? std::optional<DtuDefinition>{} : dtus_.find(session.dtuKey);
        producer_.updateSession(session.connectionId,
                                {{"connection_id", session.connectionId},
                                 {"link_id", session.linkId},
                                 {"remote_address", session.remoteAddress},
                                 {"dtu_key", session.dtuKey},
                                 {"protocol", dtu ? dtu->protocol : ""},
                                 {"state", session.dtuKey.empty() ? "connected" : "registered"},
                                 {"connected_at_ms", std::to_string(session.connectedAtMs)},
                                 {"last_seen_at_ms", std::to_string(session.lastSeenAtMs)}});
    }

    void bind(const DtuSession& session, const DtuDefinition& dtu) {
        const auto result = sessions_.bind(session.connectionId, dtu.key);
        if (!result.bound)
            return;
        if (!result.displacedConnectionId.empty() && displacedConnectionHandler_)
            displacedConnectionHandler_(result.displacedConnectionId);
        producer_.updateSession(
            session.connectionId,
            {{"connection_id", session.connectionId},
             {"link_id", session.linkId},
             {"remote_address", session.remoteAddress},
             {"dtu_key", dtu.key},
             {"protocol", dtu.protocol},
             {"state", "registered"},
             {"last_seen_at_ms", std::to_string(bridge::utcNowMilliseconds())}});
    }

    void deadLetter(const bridge::IngressPacket& packet, std::string_view reason) {
        producer_.publish(bridge::kDeadLetterStream,
                          {{"message_id", bridge::nextMessageId()},
                           {"causation_id", packet.messageId},
                           {"reason", std::string(reason)},
                           {"link_id", packet.linkId},
                           {"connection_id", packet.connectionId},
                           {"remote_address", packet.remoteAddress},
                           {"occurred_at_ms", std::to_string(packet.occurredAtMs)},
                           {"payload_hex", bridge::toHex(packet.payload)}});
    }

    DtuRegistry& dtus_;
    SessionRegistry& sessions_;
    ProtocolRegistry& protocols_;
    bridge::RedisStreamProducer& producer_;
    std::function<void(std::string_view)> displacedConnectionHandler_;
    std::mutex bufferMutex_;
    std::map<std::string, std::vector<std::uint8_t>> receiveBuffers_;
    static constexpr std::size_t kMaximumReceiveBuffer = 64 * 1024;
};

} // namespace service::southbridge
