#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/message.h"
#include "service/features/collector/collector.types.h"

namespace service::collector {

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
    message::ParsedDeviceMessage parsed;
};

struct ProtocolInput {
    std::string_view messageId;
    std::string_view linkId;
    std::string_view connectionId;
    std::string_view remoteAddress;
    std::int64_t receivedAtMs = 0;
    std::span<const std::uint8_t> bytes;
};

struct CommandElementValue {
    std::string elementId;
    std::string value;
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
    std::vector<CommandElementValue> elements;
    bool highPriority = true;
    bool expectsResponse = true;
    std::chrono::milliseconds timeout{3000};
};

class ProtocolSession {
  public:
    virtual ~ProtocolSession() = default;

    virtual void inheritTransportState(const ProtocolSession&) {}
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
                  std::string_view targetId,
                  const std::shared_ptr<const RuntimeSnapshot>& snapshot) const = 0;
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

} // namespace service::collector

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <locale>
#include <set>
#include <sstream>


namespace service::collector::command {

struct ResolvedElement {
    const ElementDefinition* definition = nullptr;
    std::string value;
};

struct ResolvedCommand {
    std::string functionCode;
    std::vector<ResolvedElement> elements;
};

inline std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

template <typename Number> Number integer(std::string_view value, std::string_view name) {
    Number result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::invalid_argument("command_invalid: " + std::string(name) +
                                    " must be an integer");
    return result;
}

inline double decimal(std::string_view value, std::string_view name) {
    double result{};
    std::istringstream input{std::string(value)};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> result;
    char trailing{};
    if (!input || input >> trailing || !std::isfinite(result))
        throw std::invalid_argument("command_invalid: " + std::string(name) +
                                    " must be a finite number");
    return result;
}

inline void validateValue(const ElementDefinition& element, std::string_view value) {
    const auto name = element.name.empty() ? element.id : element.name;
    if (value.empty())
        throw std::invalid_argument("command_invalid: " + name + " value is empty");
    const auto type = element.dataType;
    if (type == "BOOL") {
        if (value != "0" && value != "1")
            throw std::invalid_argument("command_invalid: " + name + " BOOL must be 0 or 1");
        return;
    }
    if (type == "INT8") {
        const auto parsed = integer<std::int64_t>(value, name);
        if (parsed < -128 || parsed > 127)
            throw std::invalid_argument("command_invalid: " + name + " INT8 out of range");
        return;
    }
    if (type == "UINT8" || type == "BYTE") {
        const auto parsed = integer<std::uint64_t>(value, name);
        if (parsed > 255)
            throw std::invalid_argument("command_invalid: " + name + " UINT8 out of range");
        return;
    }
    if (type == "INT16") {
        const auto parsed = integer<std::int64_t>(value, name);
        if (parsed < -32768 || parsed > 32767)
            throw std::invalid_argument("command_invalid: " + name + " INT16 out of range");
        return;
    }
    if (type == "UINT16" || type == "WORD") {
        const auto parsed = integer<std::uint64_t>(value, name);
        if (parsed > 65535)
            throw std::invalid_argument("command_invalid: " + name + " UINT16 out of range");
        return;
    }
    if (type == "INT32") {
        const auto parsed = integer<std::int64_t>(value, name);
        if (parsed < std::numeric_limits<std::int32_t>::min() ||
            parsed > std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("command_invalid: " + name + " INT32 out of range");
        return;
    }
    if (type == "UINT32" || type == "DWORD") {
        const auto parsed = integer<std::uint64_t>(value, name);
        if (parsed > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("command_invalid: " + name + " UINT32 out of range");
        return;
    }
    if (type == "INT64") {
        (void)integer<std::int64_t>(value, name);
        return;
    }
    if (type == "UINT64") {
        (void)integer<std::uint64_t>(value, name);
        return;
    }
    if (type == "FLOAT" || type == "FLOAT32" || type == "REAL") {
        const auto parsed = static_cast<float>(decimal(value, name));
        if (!std::isfinite(parsed))
            throw std::invalid_argument("command_invalid: " + name + " FLOAT out of range");
        return;
    }
    if (type == "DOUBLE" || type == "LREAL") {
        (void)decimal(value, name);
        return;
    }
    if (type == "STRING") {
        if (element.size <= 0 || value.size() > static_cast<std::size_t>(element.size))
            throw std::invalid_argument("command_invalid: " + name + " STRING is too long");
        return;
    }
    if (element.encoding == "BCD") {
        const auto parsed = decimal(value, name);
        if (parsed < 0)
            throw std::invalid_argument("command_invalid: " + name +
                                        " BCD must not be negative");
        const auto digits = std::clamp<std::int64_t>(element.digits, 0, 8);
        const auto length = std::max<std::int64_t>(1, element.length);
        const auto scaled = std::round(parsed * std::pow(10.0, digits));
        if (scaled >= std::pow(10.0, length * 2))
            throw std::invalid_argument("command_invalid: " + name + " BCD is too long");
        return;
    }
    if (!element.encoding.empty()) {
        if (value.size() >
                static_cast<std::size_t>(std::max<std::int64_t>(1, element.length) * 2) ||
            !std::ranges::all_of(
                value, [](unsigned char character) { return std::isxdigit(character) != 0; }))
            throw std::invalid_argument("command_invalid: " + name + " HEX is invalid");
        return;
    }
    (void)decimal(value, name);
}

inline ResolvedCommand resolve(const DeviceDefinition& device,
                               std::span<const CommandElementValue> requested) {
    if (requested.empty() || requested.size() > 256)
        throw std::invalid_argument("command_invalid: element count must be between 1 and 256");
    ResolvedCommand result;
    std::set<std::string, std::less<>> ids;
    for (const auto& input : requested) {
        const auto value = trim(input.value);
        if (input.elementId.empty() || !ids.insert(input.elementId).second)
            throw std::invalid_argument("command_invalid: element id is empty or duplicated");
        const auto matched =
            std::find_if(device.elements.begin(), device.elements.end(), [&](const auto& element) {
                return element.id == input.elementId && !element.responseElement;
            });
        if (matched == device.elements.end())
            throw std::invalid_argument("command_invalid: element is not configured");
        if ((device.protocol == "Modbus" || device.protocol == "S7") && !matched->writable)
            throw std::invalid_argument("command_invalid: element is not writable");
        if (device.protocol == "SL651") {
            if (matched->direction != "DOWN" || matched->encoding == "JPEG")
                throw std::invalid_argument("command_invalid: SL651 element is not writable");
            if (result.functionCode.empty())
                result.functionCode = matched->functionCode;
            else if (result.functionCode != matched->functionCode)
                throw std::invalid_argument(
                    "command_invalid: SL651 elements must share one function code");
        }
        validateValue(*matched, value);
        result.elements.push_back({&*matched, std::string(value)});
    }
    if (device.protocol == "SL651") {
        std::size_t required = 0;
        for (const auto& element : device.elements)
            if (!element.responseElement && element.direction == "DOWN" &&
                element.functionCode == result.functionCode)
                ++required;
        if (required == 0 || required != result.elements.size())
            throw std::invalid_argument(
                "command_invalid: SL651 command requires every element in the function");
    } else if (device.protocol != "Modbus" && device.protocol != "S7") {
        throw std::invalid_argument("command_invalid: unsupported protocol");
    }
    return result;
}

} // namespace service::collector::command
