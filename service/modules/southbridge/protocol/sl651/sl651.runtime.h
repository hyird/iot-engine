#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/modules/southbridge/protocol/protocol_runtime.h"
#include "service/modules/southbridge/protocol/command_value.h"

namespace service::southbridge::sl651 {

namespace detail {

inline std::uint16_t readBe16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                      bytes[offset + 1]);
}

inline std::uint16_t crc16Modbus(std::span<const std::uint8_t> bytes) noexcept {
    std::uint16_t crc = 0xFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) != 0 ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U)
                                  : static_cast<std::uint16_t>(crc >> 1U);
    }
    return crc;
}

inline std::optional<std::string> bcd(std::span<const std::uint8_t> bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto high = static_cast<std::uint8_t>(byte >> 4U);
        const auto low = static_cast<std::uint8_t>(byte & 0x0FU);
        if (high > 9 || low > 9)
            return std::nullopt;
        result.push_back(static_cast<char>('0' + high));
        result.push_back(static_cast<char>('0' + low));
    }
    return result;
}

inline std::string hexByte(std::uint8_t byte) {
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    return {digits[byte >> 4U], digits[byte & 0x0FU]};
}

inline std::string base64(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t offset = 0;
    while (offset + 3 <= bytes.size()) {
        const auto value = (static_cast<std::uint32_t>(bytes[offset]) << 16U) |
                           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
                           bytes[offset + 2];
        result.push_back(alphabet[(value >> 18U) & 0x3FU]);
        result.push_back(alphabet[(value >> 12U) & 0x3FU]);
        result.push_back(alphabet[(value >> 6U) & 0x3FU]);
        result.push_back(alphabet[value & 0x3FU]);
        offset += 3;
    }
    const auto remaining = bytes.size() - offset;
    if (remaining == 1) {
        const auto value = static_cast<std::uint32_t>(bytes[offset]) << 16U;
        result.push_back(alphabet[(value >> 18U) & 0x3FU]);
        result.push_back(alphabet[(value >> 12U) & 0x3FU]);
        result += "==";
    } else if (remaining == 2) {
        const auto value = (static_cast<std::uint32_t>(bytes[offset]) << 16U) |
                           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U);
        result.push_back(alphabet[(value >> 18U) & 0x3FU]);
        result.push_back(alphabet[(value >> 12U) & 0x3FU]);
        result.push_back(alphabet[(value >> 6U) & 0x3FU]);
        result.push_back('=');
    }
    return result;
}

inline std::string jsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const auto character : value) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) >= 0x20)
                result.push_back(character);
        }
    }
    return result;
}

inline std::vector<std::uint8_t> hexBytes(std::string_view value) { return bridge::fromHex(value); }

inline std::uint8_t bcdByte(unsigned value) {
    return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

inline std::vector<std::uint8_t> bcdAddress(std::string_view value, std::size_t bytes) {
    if (value.size() > bytes * 2 || !std::ranges::all_of(value, [](unsigned char character) {
            return std::isdigit(character);
        }))
        throw std::invalid_argument("command_invalid: SL651 device code is invalid");
    std::string padded(bytes * 2 - value.size(), '0');
    padded.append(value);
    std::vector<std::uint8_t> result;
    result.reserve(bytes);
    for (std::size_t index = 0; index < padded.size(); index += 2)
        result.push_back(
            static_cast<std::uint8_t>(((padded[index] - '0') << 4U) | (padded[index + 1] - '0')));
    return result;
}

inline std::vector<std::uint8_t> encodeValue(const ElementDefinition& element,
                                             std::string_view value) {
    const auto length = static_cast<std::size_t>(std::max<std::int64_t>(1, element.length));
    if (element.encoding == "BCD") {
        const auto parsed = command_value::decimal(value, element.name);
        const auto digits = std::clamp<std::int64_t>(element.digits, 0, 8);
        const auto scaled = static_cast<std::uint64_t>(
            std::llround(std::abs(parsed) * std::pow(10.0, static_cast<double>(digits))));
        auto decimalValue = std::to_string(scaled);
        if (decimalValue.size() > length * 2)
            throw std::invalid_argument("command_invalid: SL651 BCD is too long");
        decimalValue.insert(decimalValue.begin(), length * 2 - decimalValue.size(), '0');
        return bcdAddress(decimalValue, length);
    }
    std::string hex(value);
    hex.insert(hex.begin(), length * 2 - hex.size(), '0');
    auto result = bridge::fromHex(hex);
    if (result.size() != length)
        throw std::invalid_argument("command_invalid: SL651 HEX is invalid");
    return result;
}

inline std::chrono::seconds timezoneOffset(std::string_view timezone) {
    if (timezone.size() != 6 || (timezone[0] != '+' && timezone[0] != '-') || timezone[3] != ':')
        return std::chrono::hours(8);
    const auto hours = (timezone[1] - '0') * 10 + timezone[2] - '0';
    const auto minutes = (timezone[4] - '0') * 10 + timezone[5] - '0';
    const auto seconds = std::chrono::hours(hours) + std::chrono::minutes(minutes);
    return timezone[0] == '-' ? -seconds : seconds;
}

inline std::array<std::uint8_t, 6> reportTime(std::string_view timezone) {
    using namespace std::chrono;
    const auto local = floor<seconds>(system_clock::now()) + timezoneOffset(timezone);
    const auto day = floor<days>(local);
    const year_month_day date{day};
    const hh_mm_ss time{local - day};
    return {bcdByte(static_cast<unsigned>(static_cast<int>(date.year()) % 100)),
            bcdByte(static_cast<unsigned>(date.month())),
            bcdByte(static_cast<unsigned>(date.day())),
            bcdByte(static_cast<unsigned>(time.hours().count())),
            bcdByte(static_cast<unsigned>(time.minutes().count())),
            bcdByte(static_cast<unsigned>(time.seconds().count()))};
}

inline std::string elementValue(std::span<const std::uint8_t> bytes,
                                const ElementDefinition& element) {
    if (element.encoding == "BCD") {
        const auto value = bcd(bytes);
        if (!value)
            return {};
        if (element.digits <= 0)
            return *value;
        const auto digits = static_cast<std::size_t>(element.digits);
        if (digits >= value->size())
            return "0." + std::string(digits - value->size(), '0') + *value;
        return value->substr(0, value->size() - digits) + '.' +
               value->substr(value->size() - digits);
    }
    if (element.encoding == "TIME_YYMMDDHHMMSS") {
        const auto value = bcd(bytes);
        if (!value || value->size() < 10)
            return {};
        return "20" + value->substr(0, 2) + '-' + value->substr(2, 2) + '-' + value->substr(4, 2) +
               'T' + value->substr(6, 2) + ':' + value->substr(8, 2) + ':' +
               (value->size() >= 12 ? value->substr(10, 2) : "00");
    }
    if (element.encoding == "JPEG") {
        if (bytes.size() <= 2 || bytes[0] != 0xFF || bytes[1] != 0xD8)
            return "INVALID_JPEG";
        return "data:image/jpeg;base64," + base64(bytes);
    }
    return bridge::toHex(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

inline std::optional<std::int64_t> reportTimeMilliseconds(std::span<const std::uint8_t> body,
                                                          std::string_view timezone) {
    if (body.size() < 8 || timezone.size() != 6 ||
        (timezone.front() != '+' && timezone.front() != '-') || timezone[3] != ':')
        return std::nullopt;
    const auto timestamp = bcd(body.subspan(2, 6));
    if (!timestamp || timestamp->size() != 12)
        return std::nullopt;
    const auto pair = [&](std::size_t offset) {
        return ((*timestamp)[offset] - '0') * 10 + ((*timestamp)[offset + 1] - '0');
    };
    const auto year = 2000 + pair(0);
    const auto month = pair(2);
    const auto day = pair(4);
    const auto hour = pair(6);
    const auto minute = pair(8);
    const auto second = pair(10);
    static constexpr std::array<int, 12> monthDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const auto leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month < 1 || month > 12 || day < 1 ||
        day > monthDays[static_cast<std::size_t>(month - 1)] + (month == 2 && leap ? 1 : 0) ||
        hour > 23 || minute > 59 || second > 59)
        return std::nullopt;
    const auto daysFromCivil = [](int civilYear, unsigned civilMonth, unsigned civilDay) {
        civilYear -= civilMonth <= 2;
        const auto era = (civilYear >= 0 ? civilYear : civilYear - 399) / 400;
        const auto yearOfEra = static_cast<unsigned>(civilYear - era * 400);
        const auto adjustedMonth = civilMonth > 2 ? civilMonth - 3 : civilMonth + 9;
        const auto dayOfYear = (153 * adjustedMonth + 2) / 5 + civilDay - 1;
        const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
        return static_cast<std::int64_t>(era) * 146097 + dayOfEra - 719468;
    };
    const auto offsetHour = (timezone[1] - '0') * 10 + (timezone[2] - '0');
    const auto offsetMinute = (timezone[4] - '0') * 10 + (timezone[5] - '0');
    if (offsetHour > 14 || offsetMinute > 59)
        return std::nullopt;
    auto offsetSeconds = (offsetHour * 60 + offsetMinute) * 60;
    if (timezone.front() == '-')
        offsetSeconds = -offsetSeconds;
    const auto localSeconds =
        daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
        hour * 3600 + minute * 60 + second;
    return (localSeconds - offsetSeconds) * 1000;
}

} // namespace detail

class Session final : public ProtocolSession,
                      public CommandCapabilitySession,
                      public DeadlineCapabilitySession {
  public:
    Session(LinkDefinition link, std::string connectionId, std::vector<DeviceDefinition> devices)
        : link_(std::move(link)), connectionId_(std::move(connectionId)),
          devices_(std::move(devices)) {
        for (const auto& device : devices_)
            devicesByCode_.emplace(normalizeCode(device.code), &device);
    }

    [[nodiscard]] std::vector<ProtocolAction> consume(const ProtocolInput& input) override {
        receiveBuffer_.insert(receiveBuffer_.end(), input.bytes.begin(), input.bytes.end());
        if (receiveBuffer_.size() > kMaximumReceiveBuffer) {
            receiveBuffer_.clear();
            return {{.kind = ProtocolActionKind::Close,
                     .connectionId = connectionId_,
                     .reason = "sl651_receive_buffer_overflow"}};
        }

        std::vector<ProtocolAction> actions;
        while (true) {
            const auto header = std::search(receiveBuffer_.begin(), receiveBuffer_.end(),
                                            kHeader.begin(), kHeader.end());
            if (header == receiveBuffer_.end()) {
                if (!receiveBuffer_.empty() && receiveBuffer_.back() == 0x7E)
                    receiveBuffer_.erase(receiveBuffer_.begin(), receiveBuffer_.end() - 1);
                else
                    receiveBuffer_.clear();
                break;
            }
            if (header != receiveBuffer_.begin())
                receiveBuffer_.erase(receiveBuffer_.begin(), header);
            if (receiveBuffer_.size() < kHeaderLength)
                break;
            const auto bodyLength =
                static_cast<std::size_t>(detail::readBe16(receiveBuffer_, 11) & 0x0FFFU);
            const auto frameLength = kHeaderLength + 1 + bodyLength + 1 + 2;
            if (bodyLength > kMaximumBodyLength) {
                receiveBuffer_.erase(receiveBuffer_.begin());
                continue;
            }
            if (receiveBuffer_.size() < frameLength)
                break;
            std::vector<std::uint8_t> frame(receiveBuffer_.begin(),
                                            receiveBuffer_.begin() +
                                                static_cast<std::ptrdiff_t>(frameLength));
            receiveBuffer_.erase(receiveBuffer_.begin(),
                                 receiveBuffer_.begin() + static_cast<std::ptrdiff_t>(frameLength));
            auto frameActions = consumeFrame(input, std::move(frame));
            actions.insert(actions.end(), std::make_move_iterator(frameActions.begin()),
                           std::make_move_iterator(frameActions.end()));
        }
        return actions;
    }

    [[nodiscard]] std::vector<ProtocolAction> disconnected(std::string_view reason) override {
        std::vector<ProtocolAction> actions;
        if (pendingCommand_) {
            actions.push_back({.kind = ProtocolActionKind::FailCommand,
                               .connectionId = connectionId_,
                               .commandId = pendingCommand_->id,
                               .reason = std::string(reason)});
            actions.push_back({.kind = ProtocolActionKind::CancelDeadline,
                               .connectionId = connectionId_,
                               .deadlineToken = pendingCommand_->deadlineToken});
        }
        for (const auto& [key, packet] : multiPackets_) {
            (void)key;
            actions.push_back({.kind = ProtocolActionKind::CancelDeadline,
                               .connectionId = connectionId_,
                               .deadlineToken = packet.deadlineToken});
        }
        pendingCommand_.reset();
        multiPackets_.clear();
        receiveBuffer_.clear();
        return actions;
    }

    [[nodiscard]] std::vector<ProtocolAction> execute(ProtocolCommand command) override {
        if (command.kind == "discovery")
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = connectionId_,
                     .commandId = std::move(command.id),
                     .reason = "sl651_discovery_not_supported"}};
        if (pendingCommand_)
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = connectionId_,
                     .commandId = std::move(command.id),
                     .reason = "sl651_command_busy"}};
        const auto* device = findDevice(command);
        if (!device)
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = connectionId_,
                     .commandId = std::move(command.id),
                     .reason = "sl651_device_offline"}};
        if (command.payload.empty() && !command.elements.empty()) {
            try {
                compileElementCommand(*device, command);
            } catch (const std::exception& error) {
                return {{.kind = ProtocolActionKind::FailCommand,
                         .connectionId = connectionId_,
                         .deviceId = device->id,
                         .deviceCode = device->code,
                         .commandId = std::move(command.id),
                         .reason = error.what()}};
            }
        }
        if (command.payload.size() < kHeaderLength || command.payload[0] != 0x7E ||
            command.payload[1] != 0x7E)
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = connectionId_,
                     .commandId = std::move(command.id),
                     .reason = "sl651_command_frame_invalid"}};
        const auto remoteCode =
            detail::bcd(std::span<const std::uint8_t>(command.payload).subspan(2, 5));
        if (!remoteCode || normalizeCode(command.deviceCode) != *remoteCode)
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = connectionId_,
                     .commandId = std::move(command.id),
                     .reason = "sl651_command_device_mismatch"}};

        const auto token = nextDeadlineToken_++;
        const auto timeout = std::clamp(command.timeout, std::chrono::milliseconds(100),
                                        std::chrono::milliseconds(60000));
        pendingCommand_ = PendingCommand{command.id, *remoteCode, command.payload[10], token};
        return {{.kind = ProtocolActionKind::Send,
                 .connectionId = connectionId_,
                 .commandId = command.id,
                 .bytes = std::move(command.payload)},
                {.kind = ProtocolActionKind::ScheduleDeadline,
                 .connectionId = connectionId_,
                 .commandId = command.id,
                 .deadlineToken = token,
                 .deadlineAfter = timeout}};
    }

    [[nodiscard]] std::vector<ProtocolAction> deadline(std::uint64_t token) override {
        if (pendingCommand_ && pendingCommand_->deadlineToken == token) {
            auto command = std::move(*pendingCommand_);
            pendingCommand_.reset();
            return {{.kind = ProtocolActionKind::FailCommand,
                     .connectionId = connectionId_,
                     .commandId = std::move(command.id),
                     .reason = "sl651_command_timeout"}};
        }
        for (auto current = multiPackets_.begin(); current != multiPackets_.end(); ++current) {
            if (current->second.deadlineToken != token)
                continue;
            multiPackets_.erase(current);
            break;
        }
        return {};
    }

  private:
    struct ParsedFrame {
        std::string deviceCode;
        std::uint8_t centerCode = 0;
        std::array<std::uint8_t, 2> password{};
        std::uint8_t functionCode = 0;
        bool upstream = false;
        bool multiPacket = false;
        std::uint16_t totalPackets = 0;
        std::uint16_t sequence = 0;
        std::vector<std::uint8_t> body;
        std::vector<std::vector<std::uint8_t>> rawFrames;
    };

    struct PendingCommand {
        std::string id;
        std::string deviceCode;
        std::uint8_t functionCode = 0;
        std::uint64_t deadlineToken = 0;
    };

    struct MultiPacket {
        std::uint16_t total = 0;
        std::map<std::uint16_t, std::vector<std::uint8_t>> bodies;
        std::map<std::uint16_t, std::vector<std::uint8_t>> rawFrames;
        std::uint64_t deadlineToken = 0;
    };

    struct AddressContext {
        std::uint8_t centerCode = 0;
        std::array<std::uint8_t, 2> password{};
    };

    [[nodiscard]] const DeviceDefinition*
    findDevice(const ProtocolCommand& command) const noexcept {
        if (!command.deviceId.empty()) {
            const auto current =
                std::find_if(devices_.begin(), devices_.end(),
                             [&](const auto& item) { return item.id == command.deviceId; });
            if (current != devices_.end())
                return &*current;
        }
        const auto current = devicesByCode_.find(normalizeCode(command.deviceCode));
        return current == devicesByCode_.end() ? nullptr : current->second;
    }

    void compileElementCommand(const DeviceDefinition& device, ProtocolCommand& command) {
        const auto resolved = command_value::resolve(device, command.elements);
        const auto address = addresses_.find(normalizeCode(device.code));
        if (address == addresses_.end())
            throw std::invalid_argument("command_invalid: SL651 address context is missing");
        const auto function = detail::hexBytes(resolved.functionCode);
        if (function.size() != 1)
            throw std::invalid_argument("command_invalid: SL651 function code is invalid");

        std::vector<std::uint8_t> body{static_cast<std::uint8_t>(nextSerial_ >> 8U),
                                       static_cast<std::uint8_t>(nextSerial_)};
        if (++nextSerial_ == 0)
            nextSerial_ = 1;
        const auto time = detail::reportTime(device.timezone);
        body.insert(body.end(), time.begin(), time.end());
        for (const auto& input : resolved.elements) {
            const auto guide = detail::hexBytes(input.definition->guideHex);
            if (guide.empty())
                throw std::invalid_argument("command_invalid: SL651 guide is invalid");
            body.insert(body.end(), guide.begin(), guide.end());
            auto value = detail::encodeValue(*input.definition, input.value);
            body.insert(body.end(), value.begin(), value.end());
        }
        if (body.size() > kMaximumBodyLength)
            throw std::invalid_argument("command_invalid: SL651 command body is too large");

        command.payload = {0x7E, 0x7E};
        const auto remote = detail::bcdAddress(normalizeCode(device.code), 5);
        command.payload.insert(command.payload.end(), remote.begin(), remote.end());
        command.payload.push_back(address->second.centerCode);
        command.payload.insert(command.payload.end(), address->second.password.begin(),
                               address->second.password.end());
        command.payload.push_back(function.front());
        const auto length = static_cast<std::uint16_t>(0x8000U | body.size());
        command.payload.push_back(static_cast<std::uint8_t>(length >> 8U));
        command.payload.push_back(static_cast<std::uint8_t>(length));
        command.payload.push_back(0x02);
        command.payload.insert(command.payload.end(), body.begin(), body.end());
        command.payload.push_back(0x05);
        const auto crc = detail::crc16Modbus(command.payload);
        command.payload.push_back(static_cast<std::uint8_t>(crc >> 8U));
        command.payload.push_back(static_cast<std::uint8_t>(crc));
    }

    [[nodiscard]] std::vector<ProtocolAction> consumeFrame(const ProtocolInput& input,
                                                           std::vector<std::uint8_t> frame) {
        const auto parsed = parseFrame(std::move(frame));
        if (!parsed)
            return {};
        const auto device = devicesByCode_.find(parsed->deviceCode);
        if (device == devicesByCode_.end())
            return {};
        addresses_[parsed->deviceCode] = {parsed->centerCode, parsed->password};

        std::vector<ProtocolAction> actions;
        actions.push_back({.kind = ProtocolActionKind::BindDevice,
                           .connectionId = connectionId_,
                           .deviceId = device->second->id,
                           .deviceCode = device->second->code});

        if (parsed->multiPacket) {
            auto more = consumeMulti(input, *device->second, std::move(*parsed));
            actions.insert(actions.end(), std::make_move_iterator(more.begin()),
                           std::make_move_iterator(more.end()));
        } else {
            const auto key = parsed->deviceCode + ':' + detail::hexByte(parsed->functionCode);
            const auto previous = multiPackets_.find(key);
            if (previous != multiPackets_.end()) {
                actions.push_back({.kind = ProtocolActionKind::CancelDeadline,
                                   .connectionId = connectionId_,
                                   .deadlineToken = previous->second.deadlineToken});
                multiPackets_.erase(previous);
            }
            actions.push_back(parsedAction(input, *device->second, *parsed));
        }

        if (pendingCommand_ && parsed->upstream &&
            pendingCommand_->deviceCode == parsed->deviceCode &&
            (pendingCommand_->functionCode == parsed->functionCode ||
             parsed->functionCode == 0xE1 || parsed->functionCode == 0xE2)) {
            const auto pending = std::move(*pendingCommand_);
            pendingCommand_.reset();
            actions.push_back({.kind = ProtocolActionKind::CancelDeadline,
                               .connectionId = connectionId_,
                               .deadlineToken = pending.deadlineToken});
            actions.push_back({.kind = parsed->functionCode == 0xE2
                                           ? ProtocolActionKind::FailCommand
                                           : ProtocolActionKind::CompleteCommand,
                               .connectionId = connectionId_,
                               .deviceId = device->second->id,
                               .deviceCode = device->second->code,
                               .commandId = pending.id,
                               .reason = parsed->functionCode == 0xE2 ? "sl651_negative_ack" : ""});
        }
        return actions;
    }

    [[nodiscard]] std::optional<ParsedFrame> parseFrame(std::vector<std::uint8_t> frame) const {
        if (frame.size() < kHeaderLength + 4 || frame[0] != 0x7E || frame[1] != 0x7E)
            return std::nullopt;
        const auto receivedCrc = detail::readBe16(frame, frame.size() - 2);
        if (detail::crc16Modbus(std::span<const std::uint8_t>(frame).first(frame.size() - 2)) !=
            receivedCrc)
            return std::nullopt;
        const auto lengthField = detail::readBe16(frame, 11);
        const auto bodyLength = static_cast<std::size_t>(lengthField & 0x0FFFU);
        const auto stx = frame[13];
        if (stx != 0x02 && stx != 0x16)
            return std::nullopt;
        const auto etxOffset = 14 + bodyLength;
        if (etxOffset + 2 >= frame.size() || (frame[etxOffset] != 0x03 && frame[etxOffset] != 0x05))
            return std::nullopt;

        ParsedFrame parsed;
        parsed.upstream = (lengthField & 0xF000U) == 0;
        const auto remoteOffset = parsed.upstream ? 3U : 2U;
        const auto code =
            detail::bcd(std::span<const std::uint8_t>(frame).subspan(remoteOffset, 5));
        if (!code)
            return std::nullopt;
        parsed.deviceCode = *code;
        parsed.centerCode = frame[parsed.upstream ? 2U : 7U];
        parsed.password = {frame[8], frame[9]};
        parsed.functionCode = frame[10];
        parsed.multiPacket = stx == 0x16;
        std::size_t bodyOffset = 14;
        auto actualBodyLength = bodyLength;
        if (parsed.multiPacket) {
            if (bodyLength < 3)
                return std::nullopt;
            const auto packed = (static_cast<std::uint32_t>(frame[bodyOffset]) << 16U) |
                                (static_cast<std::uint32_t>(frame[bodyOffset + 1]) << 8U) |
                                frame[bodyOffset + 2];
            parsed.totalPackets = static_cast<std::uint16_t>((packed >> 12U) & 0x0FFFU);
            parsed.sequence = static_cast<std::uint16_t>(packed & 0x0FFFU);
            if (parsed.totalPackets == 0 || parsed.sequence == 0 ||
                parsed.sequence > parsed.totalPackets || parsed.totalPackets > kMaximumPackets)
                return std::nullopt;
            bodyOffset += 3;
            actualBodyLength -= 3;
        }
        parsed.body.assign(frame.begin() + static_cast<std::ptrdiff_t>(bodyOffset),
                           frame.begin() +
                               static_cast<std::ptrdiff_t>(bodyOffset + actualBodyLength));
        parsed.rawFrames.push_back(std::move(frame));
        return parsed;
    }

    [[nodiscard]] std::vector<ProtocolAction>
    consumeMulti(const ProtocolInput& input, const DeviceDefinition& device, ParsedFrame frame) {
        const auto key = frame.deviceCode + ':' + detail::hexByte(frame.functionCode);
        auto current = multiPackets_.find(key);
        std::vector<ProtocolAction> actions;

        // SL651 has no transfer identifier. A repeated first packet after packet 1 was already
        // accepted is therefore the only unambiguous marker available for a new image/report.
        // Drop the older partial transfer immediately instead of waiting for its idle timeout.
        if (current != multiPackets_.end() && frame.sequence == 1 &&
            (current->second.bodies.contains(1) || current->second.total != frame.totalPackets)) {
            actions.push_back({.kind = ProtocolActionKind::CancelDeadline,
                               .connectionId = connectionId_,
                               .deadlineToken = current->second.deadlineToken});
            multiPackets_.erase(current);
            current = multiPackets_.end();
        }

        if (current == multiPackets_.end()) {
            const auto token = nextDeadlineToken_++;
            current =
                multiPackets_
                    .emplace(key, MultiPacket{.total = frame.totalPackets, .deadlineToken = token})
                    .first;
        } else if (current->second.total != frame.totalPackets) {
            return actions;
        } else {
            // The timeout measures inactivity between packets. Replace the previous deadline on
            // every valid packet; a late callback for the cancelled token cannot remove the new
            // assembly because deadline() matches the current token.
            actions.push_back({.kind = ProtocolActionKind::CancelDeadline,
                               .connectionId = connectionId_,
                               .deadlineToken = current->second.deadlineToken});
            current->second.deadlineToken = nextDeadlineToken_++;
        }
        current->second.bodies.insert_or_assign(frame.sequence, std::move(frame.body));
        current->second.rawFrames.insert_or_assign(frame.sequence,
                                                   std::move(frame.rawFrames.front()));
        if (current->second.bodies.size() != current->second.total) {
            actions.push_back({.kind = ProtocolActionKind::ScheduleDeadline,
                               .connectionId = connectionId_,
                               .deadlineToken = current->second.deadlineToken,
                               .deadlineAfter = kMultiPacketIdleTimeout});
            return actions;
        }

        ParsedFrame combined = std::move(frame);
        combined.multiPacket = false;
        combined.body.clear();
        combined.rawFrames.clear();
        for (std::uint16_t sequence = 1; sequence <= current->second.total; ++sequence) {
            const auto packet = current->second.bodies.find(sequence);
            const auto rawFrame = current->second.rawFrames.find(sequence);
            if (packet == current->second.bodies.end() ||
                rawFrame == current->second.rawFrames.end())
                return actions;
            combined.body.insert(combined.body.end(), packet->second.begin(), packet->second.end());
            combined.rawFrames.push_back(std::move(rawFrame->second));
        }
        multiPackets_.erase(current);
        actions.push_back(parsedAction(input, device, combined));
        return actions;
    }

    [[nodiscard]] ProtocolAction parsedAction(const ProtocolInput& input,
                                              const DeviceDefinition& device,
                                              const ParsedFrame& frame) const {
        bridge::ParsedDeviceMessage message;
        message.messageId = bridge::nextMessageId();
        message.causationId = input.messageId;
        message.linkId = link_.id;
        message.deviceId = device.id;
        message.deviceCode = device.code;
        message.protocol = "SL651";
        message.connectionId = connectionId_;
        message.occurredAtMs = input.receivedAtMs;
        message.observedAtMs = detail::reportTimeMilliseconds(frame.body, device.timezone)
                                   .value_or(input.receivedAtMs);
        message.storageInterval = std::clamp<std::int64_t>(device.storageInterval, 1, 86400);
        message.onlineWindowMs = std::clamp<std::int64_t>(device.onlineTimeout, 1, 86400) * 1000;
        message.source = "push";
        message.rawPayloads = frame.rawFrames;
        message.valuesJson = valuesJson(device, frame);
        return {.kind = ProtocolActionKind::PublishParsed,
                .connectionId = connectionId_,
                .deviceId = device.id,
                .deviceCode = device.code,
                .parsed = std::move(message)};
    }

    [[nodiscard]] static std::string valuesJson(const DeviceDefinition& device,
                                                const ParsedFrame& frame) {
        const auto functionCode = detail::hexByte(frame.functionCode);
        const auto downFunction =
            std::any_of(device.elements.begin(), device.elements.end(), [&](const auto& element) {
                return element.functionCode == functionCode && element.direction == "DOWN";
            });
        const auto hasResponseElements =
            std::any_of(device.elements.begin(), device.elements.end(), [&](const auto& element) {
                return element.functionCode == functionCode && element.responseElement;
            });
        const auto useResponseElements = frame.upstream && downFunction && hasResponseElements;
        std::ostringstream json;
        json << "{\"function_code\":\"" << functionCode << "\",\"direction\":\""
             << (frame.upstream ? "UP" : "DOWN") << '"';
        if (frame.totalPackets > 0)
            json << ",\"is_multi_packet\":true,\"total_packets\":" << frame.totalPackets;
        json << ",\"values\":{";
        bool first = true;
        std::size_t searchOffset = 0;
        for (const auto& element : device.elements) {
            if (element.functionCode != functionCode ||
                element.responseElement != useResponseElements)
                continue;
            const auto guide = detail::hexBytes(element.guideHex);
            if (guide.empty())
                continue;
            const auto found =
                std::search(frame.body.begin() + static_cast<std::ptrdiff_t>(searchOffset),
                            frame.body.end(), guide.begin(), guide.end());
            if (found == frame.body.end())
                continue;
            const auto valueOffset =
                static_cast<std::size_t>(found - frame.body.begin()) + guide.size();
            const auto length = element.length == 0 ? frame.body.size() - valueOffset
                                                    : static_cast<std::size_t>(element.length);
            if (valueOffset + length > frame.body.size())
                continue;
            const auto value = detail::elementValue(
                std::span<const std::uint8_t>(frame.body).subspan(valueOffset, length), element);
            if (!first)
                json << ',';
            first = false;
            json << '\"' << detail::jsonEscape(element.id) << "\":{\"name\":\""
                 << detail::jsonEscape(element.name) << "\",\"value\":\""
                 << detail::jsonEscape(value) << "\",\"unit\":\""
                 << detail::jsonEscape(element.unit) << "\",\"type\":\""
                 << detail::jsonEscape(element.encoding) << "\"}";
            searchOffset = valueOffset + length;
        }
        json << "}}";
        return json.str();
    }

    static std::string normalizeCode(std::string_view code) {
        return code.size() >= 10 ? std::string(code.substr(code.size() - 10))
                                 : std::string(10 - code.size(), '0') + std::string(code);
    }

    static constexpr std::array<std::uint8_t, 2> kHeader{0x7E, 0x7E};
    static constexpr std::size_t kHeaderLength = 13;
    static constexpr std::size_t kMaximumBodyLength = 4095;
    static constexpr std::size_t kMaximumPackets = 512;
    static constexpr std::size_t kMaximumReceiveBuffer = 1024 * 1024;
    static constexpr auto kMultiPacketIdleTimeout = std::chrono::seconds(30);

    LinkDefinition link_;
    std::string connectionId_;
    std::vector<DeviceDefinition> devices_;
    std::map<std::string, const DeviceDefinition*, std::less<>> devicesByCode_;
    std::map<std::string, AddressContext, std::less<>> addresses_;
    std::vector<std::uint8_t> receiveBuffer_;
    std::optional<PendingCommand> pendingCommand_;
    std::map<std::string, MultiPacket, std::less<>> multiPackets_;
    std::uint64_t nextDeadlineToken_ = 1;
    std::uint16_t nextSerial_ = 1;
};

class Runtime final : public ProtocolRuntime {
  public:
    [[nodiscard]] std::string_view protocol() const noexcept override { return "SL651"; }

    [[nodiscard]] ProtocolCapabilities capabilities() const noexcept override {
        return ProtocolCapability::TcpServer | ProtocolCapability::Commands |
               ProtocolCapability::UnsolicitedReports;
    }

    [[nodiscard]] std::unique_ptr<ProtocolSession>
    createSession(const LinkDefinition& link, std::string_view connectionId,
                  std::string_view targetId, const RuntimeSnapshot& snapshot) const override {
        if (!targetId.empty())
            throw std::invalid_argument("SL651 cannot create a TCP Client target session");
        std::vector<DeviceDefinition> devices;
        for (const auto& device : snapshot.devices)
            if (device.linkId == link.id && device.protocol == "SL651")
                devices.push_back(device);
        return std::make_unique<Session>(link, std::string(connectionId), std::move(devices));
    }
};

} // namespace service::southbridge::sl651
