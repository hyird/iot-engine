#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/runtime.types.h"

namespace service::southbridge {

enum class ProtocolCapability : std::uint32_t {
    TcpServer = 1U << 0U,
    TcpClient = 1U << 1U,
    Registration = 1U << 2U,
    Heartbeat = 1U << 3U,
    Polling = 1U << 4U,
    Discovery = 1U << 5U,
    Commands = 1U << 6U,
    UnsolicitedReports = 1U << 7U,
};

class ProtocolCapabilities final {
  public:
    constexpr ProtocolCapabilities() = default;
    constexpr explicit ProtocolCapabilities(std::uint32_t bits) : bits_(bits) {}

    [[nodiscard]] constexpr bool has(ProtocolCapability capability) const noexcept {
        return (bits_ & static_cast<std::uint32_t>(capability)) != 0;
    }

    [[nodiscard]] constexpr std::uint32_t bits() const noexcept { return bits_; }

  private:
    std::uint32_t bits_ = 0;
};

[[nodiscard]] constexpr ProtocolCapabilities operator|(ProtocolCapability left,
                                                       ProtocolCapability right) noexcept {
    return ProtocolCapabilities(static_cast<std::uint32_t>(left) |
                                static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr ProtocolCapabilities operator|(ProtocolCapabilities left,
                                                       ProtocolCapability right) noexcept {
    return ProtocolCapabilities(left.bits() | static_cast<std::uint32_t>(right));
}

enum class ProtocolActionKind {
    Send,
    Close,
    BindDevice,
    PublishParsed,
    CompleteCommand,
    FailCommand,
    ScheduleDeadline,
    CancelDeadline,
};

struct ProtocolAction {
    ProtocolActionKind kind = ProtocolActionKind::PublishParsed;
    std::string connectionId;
    std::string deviceId;
    std::string deviceCode;
    std::string commandId;
    std::string reason;
    std::uint64_t deadlineToken = 0;
    std::chrono::milliseconds deadlineAfter{0};
    std::vector<std::uint8_t> bytes;
    bridge::ParsedDeviceMessage parsed;
};

struct ProtocolInput {
    std::string_view messageId;
    std::string_view linkId;
    std::string_view connectionId;
    std::string_view remoteAddress;
    std::int64_t receivedAtMs = 0;
    std::span<const std::uint8_t> bytes;
};

struct ProtocolCommand {
    std::string id;
    std::string deviceId;
    std::string deviceCode;
    std::string transport;
    std::string kind;
    std::string protocol;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> readbackPayload;
    std::vector<std::uint8_t> expectedReadbackData;
    std::string expectedValue;
    bool highPriority = true;
    bool expectsResponse = true;
    std::chrono::milliseconds timeout{3000};
};

class ProtocolSession {
  public:
    virtual ~ProtocolSession() = default;

    [[nodiscard]] virtual std::vector<ProtocolAction> connected() { return {}; }
    [[nodiscard]] virtual std::vector<ProtocolAction> consume(const ProtocolInput& input) = 0;
    [[nodiscard]] virtual std::vector<ProtocolAction> disconnected(std::string_view reason) = 0;
};

class CommandCapabilitySession {
  public:
    virtual ~CommandCapabilitySession() = default;
    [[nodiscard]] virtual std::vector<ProtocolAction> execute(ProtocolCommand command) = 0;
};

class DeadlineCapabilitySession {
  public:
    virtual ~DeadlineCapabilitySession() = default;
    [[nodiscard]] virtual std::vector<ProtocolAction> deadline(std::uint64_t token) = 0;
};

class ProtocolRuntime {
  public:
    virtual ~ProtocolRuntime() = default;

    [[nodiscard]] virtual std::string_view protocol() const noexcept = 0;
    [[nodiscard]] virtual ProtocolCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<ProtocolSession>
    createSession(const LinkDefinition& link, std::string_view connectionId,
                  std::string_view targetId, const RuntimeSnapshot& snapshot) const = 0;
};

inline void validateProtocolLink(const ProtocolRuntime& runtime, const LinkDefinition& link) {
    const auto capabilities = runtime.capabilities();
    if (link.mode == "TCP Server" && !capabilities.has(ProtocolCapability::TcpServer))
        throw std::invalid_argument(std::string(runtime.protocol()) +
                                    " does not support TCP Server links");
    if (link.mode == "TCP Client" && !capabilities.has(ProtocolCapability::TcpClient))
        throw std::invalid_argument(std::string(runtime.protocol()) +
                                    " does not support TCP Client links");
}

} // namespace service::southbridge
