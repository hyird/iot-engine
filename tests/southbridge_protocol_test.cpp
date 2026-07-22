#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "service/common/packet_log.h"
#include "service/modules/southbridge/protocol/command_value.h"
#include "service/modules/southbridge/protocol/modbus/modbus.runtime.h"
#include "service/modules/southbridge/protocol/protocol_engine.h"
#include "service/modules/southbridge/protocol/s7/s7.runtime.h"
#include "service/modules/southbridge/protocol/sl651/sl651.runtime.h"
#include "service/modules/southbridge/runtime_config.redis.h"
#include "service/modules/southbridge/worker_timer.scheduler.h"

namespace sb = service::southbridge;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

bool has(const std::vector<sb::ProtocolAction>& actions, sb::ProtocolActionKind kind) {
    return std::any_of(actions.begin(), actions.end(),
                       [kind](const auto& action) { return action.kind == kind; });
}

const sb::ProtocolAction& first(const std::vector<sb::ProtocolAction>& actions,
                                sb::ProtocolActionKind kind) {
    const auto current = std::find_if(actions.begin(), actions.end(),
                                      [kind](const auto& action) { return action.kind == kind; });
    if (current == actions.end())
        throw std::runtime_error("expected protocol action was not emitted");
    return *current;
}

std::uint16_t crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) != 0 ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U)
                                  : static_cast<std::uint16_t>(crc >> 1U);
    }
    return crc;
}

std::vector<std::uint8_t> slFrame(std::uint8_t functionCode,
                                  std::vector<std::uint8_t> body = {0x39, 0x00, 0x12, 0x34}) {
    std::vector<std::uint8_t> frame{0x7E,
                                    0x7E,
                                    0x01,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x01,
                                    0x00,
                                    0x00,
                                    functionCode,
                                    static_cast<std::uint8_t>(body.size() >> 8U),
                                    static_cast<std::uint8_t>(body.size()),
                                    0x02};
    frame.insert(frame.end(), body.begin(), body.end());
    frame.push_back(0x03);
    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    frame.push_back(static_cast<std::uint8_t>(crc));
    return frame;
}

std::vector<std::uint8_t> slMultiFrame(std::uint8_t functionCode, std::uint16_t total,
                                       std::uint16_t sequence, std::vector<std::uint8_t> body) {
    const auto packed = (static_cast<std::uint32_t>(total) << 12U) | sequence;
    body.insert(body.begin(),
                {static_cast<std::uint8_t>(packed >> 16U), static_cast<std::uint8_t>(packed >> 8U),
                 static_cast<std::uint8_t>(packed)});
    auto frame = slFrame(functionCode, std::move(body));
    frame[13] = 0x16;
    const auto crc = crc16(std::span<const std::uint8_t>(frame).first(frame.size() - 2));
    frame[frame.size() - 2] = static_cast<std::uint8_t>(crc >> 8U);
    frame.back() = static_cast<std::uint8_t>(crc);
    return frame;
}

std::vector<std::uint8_t> modbusRead(std::uint16_t transaction, std::uint8_t function = 3) {
    return {static_cast<std::uint8_t>(transaction >> 8U),
            static_cast<std::uint8_t>(transaction),
            0,
            0,
            0,
            6,
            1,
            function,
            0,
            0,
            0,
            1};
}

std::vector<std::uint8_t> modbusReadResponse(std::uint16_t transaction, std::uint8_t function = 3,
                                             std::vector<std::uint8_t> data = {0x12, 0x34}) {
    const auto length = static_cast<std::uint16_t>(3 + data.size());
    std::vector<std::uint8_t> response{static_cast<std::uint8_t>(transaction >> 8U),
                                       static_cast<std::uint8_t>(transaction),
                                       0,
                                       0,
                                       static_cast<std::uint8_t>(length >> 8U),
                                       static_cast<std::uint8_t>(length),
                                       1,
                                       function,
                                       static_cast<std::uint8_t>(data.size())};
    response.insert(response.end(), data.begin(), data.end());
    return response;
}

std::vector<std::uint8_t> modbusWrite(std::uint16_t transaction) {
    return {static_cast<std::uint8_t>(transaction >> 8U),
            static_cast<std::uint8_t>(transaction),
            0,
            0,
            0,
            6,
            1,
            6,
            0,
            0,
            0x12,
            0x34};
}

std::vector<std::uint8_t> withModbusCrc(std::vector<std::uint8_t> frame) {
    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return frame;
}

std::vector<std::uint8_t> modbusRequest(bool tcp, std::uint16_t transaction,
                                        std::uint8_t function) {
    std::vector<std::uint8_t> pdu{1, function, 0, 0};
    if (function >= 1 && function <= 4)
        pdu.insert(pdu.end(), {0, 1});
    else if (function == 5)
        pdu.insert(pdu.end(), {0xFF, 0x00});
    else if (function == 6)
        pdu.insert(pdu.end(), {0x12, 0x34});
    else if (function == 15)
        pdu.insert(pdu.end(), {0, 8, 1, 0x01});
    else if (function == 16)
        pdu.insert(pdu.end(), {0, 2, 4, 0x12, 0x34, 0x12, 0x34});
    if (!tcp)
        return withModbusCrc(std::move(pdu));
    const auto length = static_cast<std::uint16_t>(pdu.size());
    std::vector<std::uint8_t> frame{
        static_cast<std::uint8_t>(transaction >> 8U), static_cast<std::uint8_t>(transaction), 0, 0,
        static_cast<std::uint8_t>(length >> 8U),      static_cast<std::uint8_t>(length)};
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    return frame;
}

std::vector<std::uint8_t> modbusResponse(bool tcp, std::uint16_t transaction,
                                         std::uint8_t function) {
    std::vector<std::uint8_t> pdu{1, function};
    if (function == 1 || function == 2)
        pdu.insert(pdu.end(), {1, 0x01});
    else if (function == 3 || function == 4)
        pdu.insert(pdu.end(), {2, 0x12, 0x34});
    else if (function == 5)
        pdu.insert(pdu.end(), {0, 0, 0xFF, 0x00});
    else if (function == 6)
        pdu.insert(pdu.end(), {0, 0, 0x12, 0x34});
    else if (function == 15)
        pdu.insert(pdu.end(), {0, 0, 0, 8});
    else if (function == 16)
        pdu.insert(pdu.end(), {0, 0, 0, 2});
    if (!tcp)
        return withModbusCrc(std::move(pdu));
    const auto length = static_cast<std::uint16_t>(pdu.size());
    std::vector<std::uint8_t> frame{
        static_cast<std::uint8_t>(transaction >> 8U), static_cast<std::uint8_t>(transaction), 0, 0,
        static_cast<std::uint8_t>(length >> 8U),      static_cast<std::uint8_t>(length)};
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    return frame;
}

std::vector<std::uint8_t> modbusRtuRead() { return withModbusCrc({1, 3, 0, 0, 0, 1}); }

std::vector<std::uint8_t> modbusRtuReadResponse() { return withModbusCrc({1, 3, 2, 0x12, 0x34}); }

std::vector<std::uint8_t> modbusRtuWrite() { return withModbusCrc({1, 6, 0, 0, 0x12, 0x34}); }

std::vector<std::uint8_t> s7ReadRequest(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x1F,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x01,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x0E,
            0x00,
            0x00,
            0x04,
            0x01,
            0x12,
            0x0A,
            0x10,
            0x02,
            0x00,
            0x01,
            0x00,
            0x01,
            0x84,
            0x00,
            0x00,
            0x00};
}

std::vector<std::uint8_t> s7ReadResponse(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x1B,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x03,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x02,
            0x00,
            0x06,
            0x00,
            0x00,
            0x04,
            0x01,
            0xFF,
            0x04,
            0x00,
            0x10,
            0x12,
            0x34};
}

std::vector<std::uint8_t> s7WriteRequest(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x13,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x01,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x02,
            0x00,
            0x00,
            0x05,
            0x01};
}

std::vector<std::uint8_t> s7WriteResponse(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x15,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x03,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x02,
            0x00,
            0x00,
            0x00,
            0x00,
            0x05,
            0x01};
}

sb::ProtocolRuntimeRegistry runtimes() {
    sb::ProtocolRuntimeRegistry result;
    result.add(std::make_unique<sb::modbus::Runtime>());
    result.add(std::make_unique<sb::s7::Runtime>());
    result.add(std::make_unique<sb::sl651::Runtime>());
    return result;
}

void testCapabilities() {
    require(sb::DeviceDefinition{}.timezone == "+08:00", "device timezone must default to UTC+8");
    auto registry = runtimes();
    require(registry.require("SL651").capabilities().has(sb::ProtocolCapability::TcpServer),
            "SL651 must support TCP Server");
    require(!registry.require("SL651").capabilities().has(sb::ProtocolCapability::TcpClient),
            "SL651 must reject TCP Client");
    require(!registry.require("SL651").capabilities().has(sb::ProtocolCapability::Polling),
            "SL651 must not expose polling");
    require(registry.require("Modbus").capabilities().has(sb::ProtocolCapability::Discovery),
            "Modbus discovery capability missing");
    require(registry.require("S7").capabilities().has(sb::ProtocolCapability::Polling),
            "S7 polling capability missing");
}

void testSl651() {
    sb::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-link",
                              .name = "SL",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .ip = "127.0.0.1",
                              .port = 15001,
                              .status = "enabled"});
    sb::DeviceDefinition device;
    device.id = "sl-device";
    device.code = "0000000001";
    device.linkId = "sl-link";
    device.protocol = "SL651";
    device.elements.push_back({.id = "water",
                               .name = "Water",
                               .unit = "m",
                               .functionCode = "32",
                               .guideHex = "3900",
                               .encoding = "BCD",
                               .length = 2,
                               .digits = 2});
    device.elements.push_back({.id = "request-value",
                               .name = "Request value",
                               .functionCode = "4C",
                               .direction = "DOWN",
                               .guideHex = "3900",
                               .encoding = "BCD",
                               .length = 2,
                               .digits = 2});
    device.elements.push_back({.id = "response-value",
                               .name = "Response value",
                               .functionCode = "4C",
                               .direction = "DOWN",
                               .guideHex = "3900",
                               .encoding = "BCD",
                               .length = 2,
                               .digits = 2,
                               .responseElement = true});
    snapshot.devices.push_back(device);

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "sl-connection",
                            .linkId = "sl-link",
                            .remoteAddress = "127.0.0.1:10001",
                            .sessionEpoch = 1});
    const auto frame =
        slFrame(0x32, {0x00, 0x01, 0x24, 0x01, 0x02, 0x03, 0x04, 0x05, 0x39, 0x00, 0x12, 0x34});
    service::bridge::IngressPacket packet{.messageId = "sl-ingress",
                                          .linkId = "sl-link",
                                          .connectionId = "sl-connection",
                                          .remoteAddress = "127.0.0.1:10001",
                                          .occurredAtMs = 1000};
    packet.payload.assign(frame.begin(), frame.begin() + 5);
    require(engine.consume(packet).empty(), "partial SL651 header must not emit actions");
    packet.payload.assign(frame.begin() + 5, frame.end());
    const auto actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::BindDevice), "SL651 did not bind device code");
    const auto& parsed = first(actions, sb::ProtocolActionKind::PublishParsed).parsed;
    require(parsed.rawPayloads == std::vector<std::vector<std::uint8_t>>{frame},
            "SL651 raw frame was not preserved as a one-item array");
    require(parsed.valuesJson.find("12.34") != std::string::npos,
            "SL651 BCD element was not parsed");
    require(parsed.observedAtMs == 1704135845000,
            "SL651 device-local report time was not converted from UTC+8 to UTC");

    packet.payload = slFrame(0x4C);
    const auto responseActions = engine.consume(packet);
    const auto& responseParsed =
        first(responseActions, sb::ProtocolActionKind::PublishParsed).parsed;
    require(responseParsed.valuesJson.find("response-value") != std::string::npos &&
                responseParsed.valuesJson.find("request-value") == std::string::npos,
            "SL651 upstream response did not use responseElements");

    auto commandFrame = slFrame(0x4C, {});
    // Downlink direction occupies the high nibble of the length field; refresh the CRC.
    commandFrame[2] = 0x00;
    commandFrame[6] = 0x01;
    commandFrame[11] = 0x80;
    const auto commandCrc =
        crc16(std::span<const std::uint8_t>(commandFrame).first(commandFrame.size() - 2));
    commandFrame[commandFrame.size() - 2] = static_cast<std::uint8_t>(commandCrc >> 8U);
    commandFrame.back() = static_cast<std::uint8_t>(commandCrc);
    auto commandActions = engine.execute("sl-connection", {.id = "sl-command",
                                                           .deviceId = "sl-device",
                                                           .deviceCode = "0000000001",
                                                           .kind = "control",
                                                           .payload = commandFrame});
    require(has(commandActions, sb::ProtocolActionKind::Send), "SL651 command was not sent");
    packet.payload = slFrame(0x32);
    commandActions = engine.consume(packet);
    require(!has(commandActions, sb::ProtocolActionKind::CompleteCommand),
            "SL651 unsolicited report completed a command");
    packet.payload = slFrame(0xE1, {});
    commandActions = engine.consume(packet);
    require(has(commandActions, sb::ProtocolActionKind::CompleteCommand),
            "SL651 ACK did not complete command");

    commandActions = engine.execute(
        "sl-connection", {.id = "sl-element-command",
                          .deviceId = "sl-device",
                          .deviceCode = "0000000001",
                          .kind = "command",
                          .elements = {{.elementId = "request-value", .value = "12.34"}}});
    const auto& generatedSl651 = first(commandActions, sb::ProtocolActionKind::Send).bytes;
    require(generatedSl651[2] == 0x00 && generatedSl651[6] == 0x01 && generatedSl651[7] == 0x01 &&
                generatedSl651[10] == 0x4C,
            "SL651 element command did not reuse the observed address header");
    const std::array<std::uint8_t, 4> encodedSl651Value{0x39, 0x00, 0x12, 0x34};
    require(std::search(generatedSl651.begin(), generatedSl651.end(), encodedSl651Value.begin(),
                        encodedSl651Value.end()) != generatedSl651.end(),
            "SL651 element command did not encode its guide and BCD value");
    packet.payload = slFrame(0xE1, {});
    commandActions = engine.consume(packet);
    require(has(commandActions, sb::ProtocolActionKind::CompleteCommand),
            "generated SL651 command did not complete on ACK");

    commandActions = engine.execute("sl-connection", {.id = "sl-negative-command",
                                                      .deviceId = "sl-device",
                                                      .deviceCode = "0000000001",
                                                      .kind = "control",
                                                      .payload = commandFrame});
    require(has(commandActions, sb::ProtocolActionKind::Send),
            "SL651 negative-ack test command was not sent");
    packet.payload = slFrame(0xE2, {});
    commandActions = engine.consume(packet);
    require(first(commandActions, sb::ProtocolActionKind::FailCommand).reason ==
                "sl651_negative_ack",
            "SL651 negative ACK did not fail with a precise reason");
}

void testSl651AllEncodingsAndFunctionCodes() {
    sb::ElementDefinition element;
    element.encoding = "BCD";
    element.digits = 2;
    const std::array<std::uint8_t, 2> bcd{0x12, 0x34};
    require(sb::sl651::detail::elementValue(bcd, element) == "12.34", "SL651 BCD encoding failed");
    element.encoding = "TIME_YYMMDDHHMMSS";
    const std::array<std::uint8_t, 6> time{0x24, 0x01, 0x02, 0x03, 0x04, 0x05};
    require(sb::sl651::detail::elementValue(time, element) == "2024-01-02T03:04:05",
            "SL651 time encoding failed");
    const std::array<std::uint8_t, 3> binary{0x00, 0xAB, 0xFF};
    for (const auto encoding : {"HEX", "DICT"}) {
        element.encoding = encoding;
        require(sb::sl651::detail::elementValue(binary, element) == "00ABFF",
                "SL651 binary encoding failed");
    }
    element.encoding = "JPEG";
    require(sb::sl651::detail::elementValue(binary, element) == "INVALID_JPEG",
            "SL651 accepted an invalid JPEG");
    const std::array<std::uint8_t, 4> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    require(sb::sl651::detail::elementValue(jpeg, element) == "data:image/jpeg;base64,/9j/2Q==",
            "SL651 JPEG was not stored as a Base64 data URL");

    sb::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-function-link",
                              .name = "SL function matrix",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .status = "enabled"});
    sb::DeviceDefinition device;
    device.id = "sl-function-device";
    device.code = "0000000001";
    device.linkId = "sl-function-link";
    device.protocol = "SL651";
    for (std::uint16_t function = 0; function <= 0xFF; ++function) {
        const auto code = sb::sl651::detail::hexByte(static_cast<std::uint8_t>(function));
        device.elements.push_back({.id = "fc-" + code,
                                   .name = "Function " + code,
                                   .functionCode = code,
                                   .guideHex = "3900",
                                   .encoding = "HEX",
                                   .length = 1});
    }
    snapshot.devices.push_back(std::move(device));

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "sl-function-connection",
                            .linkId = "sl-function-link",
                            .sessionEpoch = 1});
    service::bridge::IngressPacket packet{.messageId = "sl-function-frame",
                                          .linkId = "sl-function-link",
                                          .connectionId = "sl-function-connection",
                                          .occurredAtMs = 7000};
    for (std::uint16_t function = 0; function <= 0xFF; ++function) {
        const auto code = sb::sl651::detail::hexByte(static_cast<std::uint8_t>(function));
        packet.payload = slFrame(static_cast<std::uint8_t>(function),
                                 {0x39, 0x00, static_cast<std::uint8_t>(function)});
        const auto actions = engine.consume(packet);
        const auto& parsed = first(actions, sb::ProtocolActionKind::PublishParsed).parsed;
        require(parsed.valuesJson.find("fc-" + code) != std::string::npos,
                "SL651 configured function code was not routed");
    }
}

void testSl651MultiPacketImages() {
    sb::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-image-link",
                              .name = "SL image",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .status = "enabled"});
    sb::DeviceDefinition device;
    device.id = "sl-image-device";
    device.code = "0000000001";
    device.linkId = "sl-image-link";
    device.protocol = "SL651";
    device.elements.push_back({.id = "image",
                               .name = "Image",
                               .functionCode = "36",
                               .guideHex = "3900",
                               .encoding = "JPEG",
                               .length = 0});
    snapshot.devices.push_back(std::move(device));

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected(
        {.connectionId = "sl-image-connection", .linkId = "sl-image-link", .sessionEpoch = 1});
    service::bridge::IngressPacket packet{.messageId = "sl-image-frame",
                                          .linkId = "sl-image-link",
                                          .connectionId = "sl-image-connection",
                                          .occurredAtMs = 8000};

    const auto firstFrame = slMultiFrame(0x36, 3, 1, {0x00, 0x01, 0x24, 0x01, 0x02});
    const auto secondFrame = slMultiFrame(0x36, 3, 2, {0x03, 0x04, 0x05, 0x39, 0x00, 0xFF});
    packet.payload = secondFrame;
    auto actions = engine.consume(packet);
    require(!has(actions, sb::ProtocolActionKind::PublishParsed),
            "SL651 published an incomplete image");
    const auto firstToken = first(actions, sb::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    packet.payload = firstFrame;
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::CancelDeadline),
            "SL651 did not cancel the previous image idle deadline");
    const auto secondToken = first(actions, sb::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    require(secondToken != firstToken, "SL651 did not refresh the image idle deadline");
    require(engine.deadline("sl-image-connection", firstToken).empty(),
            "a stale SL651 image deadline affected the refreshed assembly");

    const auto thirdFrame = slMultiFrame(0x36, 3, 3, {0xD8, 0xFF, 0xD9});
    packet.payload = thirdFrame;
    actions = engine.consume(packet);
    const auto& parsed = first(actions, sb::ProtocolActionKind::PublishParsed).parsed;
    require(parsed.valuesJson.find("data:image/jpeg;base64,/9j/2Q==") != std::string::npos,
            "SL651 multi-packet image was not assembled and encoded");
    require(parsed.valuesJson.find("\"type\":\"JPEG\"") != std::string::npos,
            "SL651 image storage omitted the JPEG type");
    require(parsed.valuesJson.find("\"is_multi_packet\":true") != std::string::npos &&
                parsed.valuesJson.find("\"total_packets\":3") != std::string::npos,
            "SL651 image storage omitted multi-packet metadata");
    const std::vector<std::vector<std::uint8_t>> expectedRaw{firstFrame, secondFrame, thirdFrame};
    require(parsed.rawPayloads == expectedRaw,
            "SL651 multi-packet raw frames were not stored in sequence order");
    const auto streamFields = service::bridge::parsedFields(parsed);
    service::bridge::StreamMessage streamMessage{.id = "1-0", .fields = streamFields};
    const auto roundTrip = service::bridge::parsedFrom(streamMessage);
    require(roundTrip.rawPayloads == expectedRaw &&
                streamMessage.get("raw_payload_hex") ==
                    service::bridge::rawPayloadsJson(expectedRaw),
            "parsed Redis message did not preserve the ordered HEX payload array");

    const auto oldFirst = slMultiFrame(0x36, 2, 1, {0x00, 0x01, 0x24, 0x01});
    packet.payload = oldFirst;
    actions = engine.consume(packet);
    const auto oldToken = first(actions, sb::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    const auto newFirst = slMultiFrame(
        0x36, 2, 1, {0x00, 0x01, 0x24, 0x01, 0x02, 0x03, 0x04, 0x05, 0x39, 0x00, 0xFF, 0xD8});
    packet.payload = newFirst;
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::CancelDeadline).deadlineToken == oldToken,
            "a new SL651 image did not replace the previous partial image");
    require(engine.deadline("sl-image-connection", oldToken).empty(),
            "the replaced SL651 image deadline affected the new image");
    const auto newLast = slMultiFrame(0x36, 2, 2, {0xFF, 0xD9});
    packet.payload = newLast;
    actions = engine.consume(packet);
    const auto& replacement = first(actions, sb::ProtocolActionKind::PublishParsed).parsed;
    const std::vector<std::vector<std::uint8_t>> replacementRaw{newFirst, newLast};
    require(replacement.rawPayloads == replacementRaw,
            "the replaced SL651 image leaked old raw packets into storage");
}

void testModbus() {
    sb::RuntimeSnapshot snapshot;
    sb::LinkDefinition link{.id = "modbus-link",
                            .name = "Modbus",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "target-1",
                            .name = "Target",
                            .ip = "127.0.0.1",
                            .port = 15002,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    sb::DeviceDefinition device;
    device.id = "modbus-device";
    device.code = "MODBUS-1";
    device.linkId = "modbus-link";
    device.linkMode = "TCP Client";
    device.targetId = "target-1";
    device.protocol = "Modbus";
    device.modbusMode = "TCP";
    device.slaveId = 1;
    device.elements.push_back({.id = "holding-0",
                               .name = "Holding",
                               .unit = "V",
                               .dataType = "UINT16",
                               .byteOrder = "BIG_ENDIAN",
                               .registerType = "HOLDING_REGISTER",
                               .address = 0,
                               .quantity = 1,
                               .writable = true});
    device.elements.push_back({.id = "holding-2",
                               .name = "Holding 2",
                               .dataType = "UINT16",
                               .byteOrder = "BIG_ENDIAN",
                               .registerType = "HOLDING_REGISTER",
                               .address = 2,
                               .quantity = 1});
    snapshot.devices.push_back(device);

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    const auto connected = engine.connected({.connectionId = "modbus-connection",
                                             .linkId = "modbus-link",
                                             .remoteAddress = "127.0.0.1:15002",
                                             .targetId = "target-1",
                                             .sessionEpoch = 1});
    require(has(connected, sb::ProtocolActionKind::BindDevice),
            "Modbus client target was not bound");
    const auto pollToken = first(connected, sb::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    auto actions = engine.execute("modbus-connection", {.id = "modbus-read",
                                                        .deviceId = "modbus-device",
                                                        .deviceCode = "MODBUS-1",
                                                        .kind = "read",
                                                        .payload = modbusRead(1)});
    require(has(actions, sb::ProtocolActionKind::Send), "Modbus read was not dispatched");
    service::bridge::IngressPacket packet{.messageId = "modbus-ingress",
                                          .linkId = "modbus-link",
                                          .connectionId = "modbus-connection",
                                          .remoteAddress = "127.0.0.1:15002",
                                          .occurredAtMs = 2000,
                                          .payload = modbusReadResponse(1)};
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::PublishParsed), "Modbus response was not parsed");
    require(has(actions, sb::ProtocolActionKind::CompleteCommand), "Modbus read did not complete");

    actions = engine.execute("modbus-connection", {.id = "invalid-write",
                                                   .deviceId = "modbus-device",
                                                   .deviceCode = "MODBUS-1",
                                                   .kind = "write",
                                                   .payload = modbusWrite(2)});
    require(first(actions, sb::ProtocolActionKind::FailCommand).reason ==
                "modbus_write_readback_required",
            "Modbus accepted a write without readback");

    actions = engine.execute("modbus-connection", {.id = "verified-write",
                                                   .deviceId = "modbus-device",
                                                   .deviceCode = "MODBUS-1",
                                                   .kind = "write",
                                                   .payload = modbusWrite(3),
                                                   .readbackPayload = modbusRead(4),
                                                   .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusWrite(3),
            "Modbus write was not sent first");
    packet.payload = modbusWrite(3);
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRead(4),
            "Modbus write response did not trigger readback");
    packet.payload = modbusReadResponse(4);
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::CompleteCommand),
            "Modbus verified write did not complete");

    actions = engine.execute("modbus-connection",
                             {.id = "element-write",
                              .deviceId = "modbus-device",
                              .deviceCode = "MODBUS-1",
                              .kind = "command",
                              .elements = {{.elementId = "holding-0", .value = "4660"}}});
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusWrite(1),
            "Modbus element command did not compile FC06");
    packet.payload = modbusWrite(1);
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRead(2),
            "Modbus element command did not compile FC03 readback");
    packet.payload = modbusReadResponse(2);
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::CompleteCommand),
            "Modbus element command did not complete after readback");

    actions = engine.deadline("modbus-connection", pollToken);
    require(has(actions, sb::ProtocolActionKind::Send),
            "Modbus periodic poll was not generated by its session");
    const auto& poll = first(actions, sb::ProtocolActionKind::Send).bytes;
    require(poll.size() == 12 && poll[7] == 3 && poll[10] == 0 && poll[11] == 3,
            "Modbus register mergeGap did not combine one read range");
}

void testModbusTypesAndPriority() {
    sb::ElementDefinition element;
    element.dataType = "UINT64";
    element.byteOrder = "BIG_ENDIAN";
    const std::array<std::uint8_t, 8> maximum{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    require(sb::modbus::detail::numericJson(maximum, element) == "18446744073709551615",
            "Modbus UINT64 lost integer precision");
    element.dataType = "UINT32";
    element.byteOrder = "BIG_ENDIAN_BYTE_SWAP";
    const std::array<std::uint8_t, 4> bigSwap{0x34, 0x12, 0x78, 0x56};
    require(sb::modbus::detail::numericJson(bigSwap, element) == "305419896",
            "Modbus BIG_ENDIAN_BYTE_SWAP was decoded incorrectly");
    element.byteOrder = "LITTLE_ENDIAN_BYTE_SWAP";
    const std::array<std::uint8_t, 4> littleSwap{0x56, 0x78, 0x12, 0x34};
    require(sb::modbus::detail::numericJson(littleSwap, element) == "305419896",
            "Modbus LITTLE_ENDIAN_BYTE_SWAP was decoded incorrectly");

    sb::RuntimeSnapshot snapshot;
    sb::LinkDefinition link{.id = "priority-link",
                            .name = "Priority",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "priority-target",
                            .name = "Target",
                            .ip = "127.0.0.1",
                            .port = 15004,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    sb::DeviceDefinition device;
    device.id = "priority-device";
    device.code = "PRIORITY-1";
    device.linkId = link.id;
    device.linkMode = link.mode;
    device.targetId = "priority-target";
    device.protocol = "Modbus";
    device.modbusMode = "TCP";
    device.elements.push_back({.id = "coil",
                               .name = "Coil",
                               .dataType = "BOOL",
                               .registerType = "COIL",
                               .address = 0,
                               .quantity = 1});
    snapshot.devices.push_back(device);

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "priority-connection",
                            .linkId = link.id,
                            .targetId = "priority-target",
                            .sessionEpoch = 1});
    auto actions = engine.execute("priority-connection", {.id = "active-read",
                                                          .deviceId = device.id,
                                                          .deviceCode = device.code,
                                                          .kind = "read",
                                                          .payload = modbusRead(10, 1)});
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRead(10, 1),
            "Modbus active read was not dispatched");
    actions = engine.execute("priority-connection", {.id = "normal-read",
                                                     .deviceId = device.id,
                                                     .deviceCode = device.code,
                                                     .kind = "read",
                                                     .payload = modbusRead(11, 1),
                                                     .highPriority = false});
    require(actions.empty(), "Modbus normal read bypassed the in-flight request");
    actions = engine.execute("priority-connection", {.id = "high-write",
                                                     .deviceId = device.id,
                                                     .deviceCode = device.code,
                                                     .kind = "write",
                                                     .payload = modbusWrite(12),
                                                     .readbackPayload = modbusRead(13),
                                                     .expectedReadbackData = {0x12, 0x34},
                                                     .highPriority = true});
    require(actions.empty(), "Modbus high write bypassed the in-flight request");
    service::bridge::IngressPacket packet{.messageId = "active-response",
                                          .linkId = link.id,
                                          .connectionId = "priority-connection",
                                          .occurredAtMs = 5000,
                                          .payload = modbusReadResponse(10, 1, {0x01})};
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::PublishParsed)
                    .parsed.valuesJson.find("\"value\":1") != std::string::npos,
            "Modbus coil response was not parsed bitwise");
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusWrite(12),
            "Modbus high-priority write did not jump ahead of a normal read");
    packet.payload = modbusWrite(12);
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRead(13),
            "Modbus high-priority write did not enter atomic readback");
    packet.payload = modbusReadResponse(13);
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRead(11, 1),
            "Modbus normal read was not resumed after write readback");
}

void testModbusAllDataTypesAndByteOrders() {
    struct TypeCase {
        std::string_view type;
        std::vector<std::uint8_t> canonical;
        std::string_view expected;
    };
    const std::array cases{
        TypeCase{"BOOL", {0x01}, "1"},
        TypeCase{"INT16", {0xFF, 0xFE}, "-2"},
        TypeCase{"UINT16", {0x12, 0x34}, "4660"},
        TypeCase{"INT32", {0xFF, 0xFF, 0xFF, 0xFE}, "-2"},
        TypeCase{"UINT32", {0x12, 0x34, 0x56, 0x78}, "305419896"},
        TypeCase{"FLOAT32", {0x3F, 0xC0, 0x00, 0x00}, "1.5"},
        TypeCase{"INT64", {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}, "-2"},
        TypeCase{"UINT64", {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}, "81985529216486895"},
        TypeCase{"DOUBLE", {0x3F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "1.5"},
    };
    const std::array<std::string_view, 4> orders{"BIG_ENDIAN", "LITTLE_ENDIAN",
                                                 "BIG_ENDIAN_BYTE_SWAP", "LITTLE_ENDIAN_BYTE_SWAP"};
    for (const auto& current : cases) {
        sb::ElementDefinition element;
        element.dataType = current.type;
        for (const auto order : orders) {
            element.byteOrder = order;
            const auto wire = sb::modbus::detail::orderedBytes(current.canonical, order);
            const auto decoded = sb::modbus::detail::numericJson(wire, element);
            require(decoded && *decoded == current.expected,
                    "Modbus data type or byte order matrix failed");
        }
    }

    sb::ElementDefinition scaled;
    scaled.dataType = "UINT16";
    scaled.byteOrder = "BIG_ENDIAN";
    scaled.scale = 0.1;
    scaled.decimals = 1;
    const std::array<std::uint8_t, 2> raw{0x04, 0xD2};
    require(sb::modbus::detail::numericJson(raw, scaled) == "123.4",
            "Modbus scale and decimals were not applied");
}

void testModbusAllFunctionCodes(bool tcp) {
    sb::RuntimeSnapshot snapshot;
    sb::LinkDefinition link{.id = tcp ? "modbus-tcp-matrix" : "modbus-rtu-matrix",
                            .name = "Modbus function matrix",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "matrix-target",
                            .name = "Matrix target",
                            .ip = "127.0.0.1",
                            .port = 15005,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    sb::DeviceDefinition device;
    device.id = tcp ? "modbus-tcp-device" : "modbus-rtu-device-matrix";
    device.code = tcp ? "MODBUS-TCP-MATRIX" : "MODBUS-RTU-MATRIX";
    device.linkId = link.id;
    device.linkMode = link.mode;
    device.targetId = "matrix-target";
    device.protocol = "Modbus";
    device.modbusMode = tcp ? "TCP" : "RTU";
    device.slaveId = 1;
    device.elements = {
        {.id = "coil", .name = "Coil", .dataType = "BOOL", .registerType = "COIL", .quantity = 1},
        {.id = "discrete",
         .name = "Discrete",
         .dataType = "BOOL",
         .registerType = "DISCRETE_INPUT",
         .quantity = 1},
        {.id = "holding",
         .name = "Holding",
         .dataType = "UINT16",
         .registerType = "HOLDING_REGISTER",
         .quantity = 1},
        {.id = "input",
         .name = "Input",
         .dataType = "UINT16",
         .registerType = "INPUT_REGISTER",
         .quantity = 1},
    };
    snapshot.devices.push_back(device);

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "modbus-matrix-connection",
                            .linkId = link.id,
                            .targetId = "matrix-target",
                            .sessionEpoch = 1});
    service::bridge::IngressPacket packet{.messageId = "modbus-matrix-response",
                                          .linkId = link.id,
                                          .connectionId = "modbus-matrix-connection",
                                          .occurredAtMs = 6000};
    std::uint16_t transaction = 100;
    for (const auto function : std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 15, 16}) {
        const auto request = modbusRequest(tcp, transaction, function);
        sb::ProtocolCommand command{
            .id = "fc-" + std::to_string(function),
            .deviceId = device.id,
            .deviceCode = device.code,
            .kind = function == 5 || function == 6 || function == 15 || function == 16 ? "write"
                                                                                       : "read",
            .payload = request};
        const auto write = command.kind == "write";
        std::uint8_t readFunction = 0;
        std::uint16_t readTransaction = 0;
        if (write) {
            readFunction = function == 5 || function == 15 ? 1 : 3;
            readTransaction = ++transaction;
            command.readbackPayload = modbusRequest(tcp, readTransaction, readFunction);
            command.expectedReadbackData = readFunction == 1
                                               ? std::vector<std::uint8_t>{0x01}
                                               : std::vector<std::uint8_t>{0x12, 0x34};
        }
        auto actions = engine.execute("modbus-matrix-connection", std::move(command));
        require(first(actions, sb::ProtocolActionKind::Send).bytes == request,
                "Modbus function code request was not dispatched");
        packet.payload = modbusResponse(tcp, transaction - (write ? 1 : 0), function);
        actions = engine.consume(packet);
        if (write) {
            require(first(actions, sb::ProtocolActionKind::Send).bytes ==
                        modbusRequest(tcp, readTransaction, readFunction),
                    "Modbus write function did not trigger readback");
            packet.payload = modbusResponse(tcp, readTransaction, readFunction);
            actions = engine.consume(packet);
        }
        require(has(actions, sb::ProtocolActionKind::CompleteCommand),
                "Modbus function code did not complete");
        ++transaction;
    }
}

void testModbusRtuZeroAddress() {
    sb::RuntimeSnapshot snapshot;
    sb::LinkDefinition link{.id = "modbus-rtu-link",
                            .name = "Modbus RTU",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "rtu-target",
                            .name = "RTU Target",
                            .ip = "127.0.0.1",
                            .port = 15003,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    sb::DeviceDefinition device;
    device.id = "modbus-rtu-device";
    device.code = "MODBUS-RTU-1";
    device.linkId = link.id;
    device.linkMode = link.mode;
    device.targetId = "rtu-target";
    device.protocol = "Modbus";
    device.modbusMode = "RTU";
    device.slaveId = 1;
    device.elements.push_back({.id = "rtu-holding-0",
                               .name = "Holding",
                               .dataType = "UINT16",
                               .registerType = "HOLDING_REGISTER",
                               .address = 0,
                               .quantity = 1});
    snapshot.devices.push_back(device);

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "modbus-rtu-connection",
                            .linkId = link.id,
                            .targetId = "rtu-target",
                            .sessionEpoch = 1});
    auto actions = engine.execute("modbus-rtu-connection", {.id = "rtu-read",
                                                            .deviceId = device.id,
                                                            .deviceCode = device.code,
                                                            .kind = "read",
                                                            .payload = modbusRtuRead()});
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRtuRead(),
            "zero-address Modbus RTU read was mistaken for TCP");
    service::bridge::IngressPacket packet{.messageId = "rtu-read-response",
                                          .linkId = link.id,
                                          .connectionId = "modbus-rtu-connection",
                                          .occurredAtMs = 3000,
                                          .payload = modbusRtuReadResponse()};
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::CompleteCommand),
            "zero-address Modbus RTU response did not complete");

    actions = engine.execute("modbus-rtu-connection", {.id = "rtu-write",
                                                       .deviceId = device.id,
                                                       .deviceCode = device.code,
                                                       .kind = "write",
                                                       .payload = modbusRtuWrite(),
                                                       .readbackPayload = modbusRtuRead(),
                                                       .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRtuWrite(),
            "zero-address Modbus RTU write was mistaken for TCP");
    packet.messageId = "rtu-write-response";
    packet.payload = modbusRtuWrite();
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::Send).bytes == modbusRtuRead(),
            "Modbus RTU write did not trigger readback");
    packet.messageId = "rtu-readback-response";
    packet.payload = modbusRtuReadResponse();
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::CompleteCommand),
            "Modbus RTU write readback did not complete");
}

void testModbusDiscoveryAndOffline() {
    sb::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "modbus-server",
                              .name = "Modbus Server",
                              .mode = "TCP Server",
                              .protocol = "Modbus",
                              .status = "enabled"});
    for (std::uint8_t unit = 1; unit <= 2; ++unit) {
        sb::DeviceDefinition device;
        device.id = "discovery-device-" + std::to_string(unit);
        device.code = "DISCOVERY" + std::to_string(unit);
        device.linkId = "modbus-server";
        device.linkMode = "TCP Server";
        device.protocol = "Modbus";
        device.modbusMode = "TCP";
        device.slaveId = unit;
        device.onlineTimeout = 1;
        snapshot.devices.push_back(std::move(device));
    }
    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected(
        {.connectionId = "discovery-connection", .linkId = "modbus-server", .sessionEpoch = 1});
    auto actions =
        engine.execute("discovery-connection", {.id = "discovery-command",
                                                .transport = "TCP",
                                                .kind = "discovery",
                                                .payload = modbusRead(11),
                                                .timeout = std::chrono::milliseconds(500)});
    const auto discoveryToken =
        first(actions, sb::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    service::bridge::IngressPacket packet{.messageId = "discovery-response-1",
                                          .linkId = "modbus-server",
                                          .connectionId = "discovery-connection",
                                          .occurredAtMs = 4000,
                                          .payload = modbusReadResponse(11)};
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::BindDevice).deviceCode == "DISCOVERY1",
            "Modbus discovery did not register the first response");
    packet.messageId = "discovery-response-2";
    packet.payload[6] = 2;
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::BindDevice).deviceCode == "DISCOVERY2",
            "Modbus discovery did not register the second response");
    actions = engine.deadline("discovery-connection", discoveryToken);
    require(has(actions, sb::ProtocolActionKind::CompleteCommand),
            "Modbus discovery window did not complete");

    sb::RuntimeSnapshot offlineSnapshot;
    offlineSnapshot.links = snapshot.links;
    auto offlineDevice = snapshot.devices.front();
    offlineDevice.registrationBytes = {'R', 'E', 'G'};
    offlineSnapshot.devices.push_back(offlineDevice);
    engine.reload(offlineSnapshot);
    (void)engine.connected(
        {.connectionId = "offline-connection", .linkId = "modbus-server", .sessionEpoch = 2});
    packet.connectionId = "offline-connection";
    packet.payload = {'R', 'E', 'G'};
    actions = engine.consume(packet);
    const auto offlineToken =
        first(actions, sb::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    actions = engine.deadline("offline-connection", offlineToken);
    require(first(actions, sb::ProtocolActionKind::Close).reason == "modbus_offline_timeout",
            "Modbus offline timeout did not close the socket with a precise reason");
}

void testS7() {
    sb::RuntimeSnapshot snapshot;
    sb::LinkDefinition link{
        .id = "s7-link", .name = "S7", .mode = "TCP Client", .protocol = "S7", .status = "enabled"};
    link.targets.push_back(
        {.id = "target-1", .name = "PLC", .ip = "127.0.0.1", .port = 102, .status = "enabled"});
    snapshot.links.push_back(link);
    sb::DeviceDefinition device;
    device.id = "s7-device";
    device.code = "S7-1";
    device.linkId = "s7-link";
    device.linkMode = "TCP Client";
    device.targetId = "target-1";
    device.protocol = "S7";
    device.elements.push_back({.id = "db1-word",
                               .name = "DB1 Word",
                               .unit = "",
                               .dataType = "UINT16",
                               .area = "DB",
                               .dbNumber = 1,
                               .start = 0,
                               .size = 2,
                               .writable = true});
    device.elements.push_back({.id = "v-lreal",
                               .name = "V LREAL",
                               .dataType = "LREAL",
                               .area = "V",
                               .start = 4,
                               .size = 8});
    device.elements.push_back({.id = "marker-float",
                               .name = "Marker float",
                               .dataType = "FLOAT",
                               .area = "MK",
                               .start = 0,
                               .size = 4});
    device.elements.push_back({.id = "input-bit",
                               .name = "Input bit",
                               .dataType = "BOOL",
                               .area = "PE",
                               .start = 2,
                               .startBit = 3,
                               .size = 1});
    device.elements.push_back({.id = "output-bit",
                               .name = "Output bit",
                               .dataType = "BOOL",
                               .area = "PA",
                               .start = 3,
                               .startBit = 2,
                               .size = 1});
    device.elements.push_back({.id = "counter",
                               .name = "Counter",
                               .dataType = "UINT16",
                               .area = "CT",
                               .start = 1,
                               .size = 2});
    device.elements.push_back({.id = "timer",
                               .name = "Timer",
                               .dataType = "UINT16",
                               .area = "TM",
                               .start = 1,
                               .size = 2});
    snapshot.devices.push_back(device);

    sb::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    auto actions = engine.connected({.connectionId = "s7-connection",
                                     .linkId = "s7-link",
                                     .remoteAddress = "127.0.0.1:102",
                                     .targetId = "target-1",
                                     .sessionEpoch = 1});
    require(has(actions, sb::ProtocolActionKind::Send), "S7 COTP request was not emitted");
    require(!has(actions, sb::ProtocolActionKind::BindDevice),
            "S7 device became online before protocol negotiation");
    service::bridge::IngressPacket packet{.messageId = "s7-ingress",
                                          .linkId = "s7-link",
                                          .connectionId = "s7-connection",
                                          .remoteAddress = "127.0.0.1:102",
                                          .occurredAtMs = 3000,
                                          .payload = {0x03, 0x00, 0x00, 0x07, 0x02, 0xD0, 0x00}};
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::Send), "S7 setup request was not emitted");
    packet.payload = {0x03, 0x00, 0x00, 0x1B, 0x02, 0xF0, 0x80, 0x32, 0x03,
                      0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00,
                      0x00, 0xF0, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0xE0};
    actions = engine.consume(packet);
    require(!has(actions, sb::ProtocolActionKind::Close), "S7 setup response was rejected");
    require(has(actions, sb::ProtocolActionKind::BindDevice),
            "S7 device was not bound after protocol negotiation");
    const auto pollToken = first(actions, sb::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    actions = engine.execute("s7-connection", {.id = "s7-read",
                                               .deviceId = "s7-device",
                                               .deviceCode = "S7-1",
                                               .kind = "read",
                                               .payload = s7ReadRequest(2)});
    require(has(actions, sb::ProtocolActionKind::Send), "S7 read was not dispatched");
    packet.payload = s7ReadResponse(2);
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::PublishParsed), "S7 response was not parsed");
    require(has(actions, sb::ProtocolActionKind::CompleteCommand), "S7 read did not complete");
    require(first(actions, sb::ProtocolActionKind::PublishParsed).parsed.valuesJson.find("4660") !=
                std::string::npos,
            "S7 DB value was not decoded");

    actions = engine.execute("s7-connection", {.id = "s7-write",
                                               .deviceId = "s7-device",
                                               .deviceCode = "S7-1",
                                               .kind = "write",
                                               .payload = s7WriteRequest(3),
                                               .readbackPayload = s7ReadRequest(4),
                                               .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, sb::ProtocolActionKind::Send).bytes == s7WriteRequest(3),
            "S7 Write Var was not dispatched");
    packet.payload = s7WriteResponse(3);
    actions = engine.consume(packet);
    require(first(actions, sb::ProtocolActionKind::Send).bytes == s7ReadRequest(4),
            "S7 Write Var did not trigger Read Var verification");
    packet.payload = s7ReadResponse(4);
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::CompleteCommand),
            "S7 Write Var readback did not complete");

    actions =
        engine.execute("s7-connection", {.id = "s7-element-write",
                                         .deviceId = "s7-device",
                                         .deviceCode = "S7-1",
                                         .kind = "command",
                                         .elements = {{.elementId = "db1-word", .value = "4660"}}});
    const auto generatedWrite = first(actions, sb::ProtocolActionKind::Send).bytes;
    require(generatedWrite.size() == 37 && generatedWrite[17] == 0x05 &&
                generatedWrite[35] == 0x12 && generatedWrite[36] == 0x34,
            "S7 element command did not compile Write Var");
    const auto writeReference =
        static_cast<std::uint16_t>(generatedWrite[11] << 8U) | generatedWrite[12];
    packet.payload = s7WriteResponse(writeReference);
    actions = engine.consume(packet);
    const auto generatedReadback = first(actions, sb::ProtocolActionKind::Send).bytes;
    require(generatedReadback[17] == 0x04,
            "S7 element command did not compile Read Var verification");
    const auto readReference =
        static_cast<std::uint16_t>(generatedReadback[11] << 8U) | generatedReadback[12];
    packet.payload = s7ReadResponse(readReference);
    actions = engine.consume(packet);
    require(has(actions, sb::ProtocolActionKind::CompleteCommand),
            "S7 element command did not complete after readback");

    actions = engine.deadline("s7-connection", pollToken);
    require(has(actions, sb::ProtocolActionKind::Send),
            "S7 periodic poll was not generated by its session");
    const auto& batched = first(actions, sb::ProtocolActionKind::Send).bytes;
    require(batched.size() == 19 + 7 * 12 && batched[18] == 7,
            "S7 poll did not batch elements within the negotiated PDU");
    std::map<std::uint8_t, std::size_t> areaCounts;
    std::map<std::uint8_t, std::uint8_t> wordLengths;
    for (std::size_t index = 0; index < batched[18]; ++index) {
        const auto offset = 19 + index * 12;
        ++areaCounts[batched[offset + 8]];
        wordLengths[batched[offset + 8]] = batched[offset + 3];
    }
    require(areaCounts[0x84] == 2 && areaCounts[0x83] == 1 && areaCounts[0x81] == 1 &&
                areaCounts[0x82] == 1 && areaCounts[0x1C] == 1 && areaCounts[0x1D] == 1,
            "S7 area mapping matrix is incomplete");
    require(wordLengths[0x1C] == 0x1C && wordLengths[0x1D] == 0x1D,
            "S7 timer/counter word length is incorrect");

    sb::ElementDefinition decoded;
    decoded.dataType = "LREAL";
    const std::array<std::uint8_t, 8> one{0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    require(sb::s7::detail::decodeJson(one, decoded) == "1", "S7 LREAL was not decoded");
    decoded.dataType = "STRING";
    const std::array<std::uint8_t, 4> text{'A', 'B', 'C', 0x00};
    require(sb::s7::detail::decodeJson(text, decoded) == "\"ABC\"", "S7 STRING was not decoded");
}

void testS7AllDataTypes() {
    struct TypeCase {
        std::string_view type;
        std::vector<std::uint8_t> bytes;
        std::string_view expected;
        std::int64_t startBit = 0;
    };
    const std::array cases{
        TypeCase{"BOOL", {0x08}, "1", 3},
        TypeCase{"INT8", {0xFE}, "-2"},
        TypeCase{"UINT8", {0xFE}, "254"},
        TypeCase{"INT16", {0xFF, 0xFE}, "-2"},
        TypeCase{"UINT16", {0x12, 0x34}, "4660"},
        TypeCase{"INT32", {0xFF, 0xFF, 0xFF, 0xFE}, "-2"},
        TypeCase{"UINT32", {0x12, 0x34, 0x56, 0x78}, "305419896"},
        TypeCase{"FLOAT", {0x3F, 0xC0, 0x00, 0x00}, "1.5"},
        TypeCase{"LREAL", {0x3F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "1.5"},
        TypeCase{"STRING", {'A', 'B', 'C', 0x00}, "\"ABC\""},
    };
    for (const auto& current : cases) {
        sb::ElementDefinition element;
        element.dataType = current.type;
        element.startBit = current.startBit;
        const auto decoded = sb::s7::detail::decodeJson(current.bytes, element);
        require(decoded && *decoded == current.expected, "S7 data type matrix failed");
    }
    const std::string invalidUtf8{static_cast<char>(0xC0), '"'};
    require(sb::s7::detail::jsonEscape(invalidUtf8) == "\\u00C0\\\"",
            "S7 invalid string bytes did not produce valid JSON escapes");
    require(sb::s7::detail::jsonEscape("中文") == "中文", "S7 valid UTF-8 text was not preserved");
}

void testWorkerTimer() {
    asio::io_context io;
    sb::WorkerTimerScheduler scheduler(io);
    int completed = 0;
    const auto cancelled =
        scheduler.scheduleAfter(std::chrono::milliseconds(1), [&] { completed += 100; });
    scheduler.cancel(cancelled);
    (void)scheduler.scheduleAfter(std::chrono::milliseconds(1), [&] { ++completed; });
    (void)scheduler.scheduleAfter(std::chrono::milliseconds(2), [&] {
        ++completed;
        scheduler.stop();
    });
    io.run();
    require(completed == 2, "worker timer cancellation or execution failed");
}

void testRuntimeConfigWritableContract() {
    sb::RuntimeSnapshot readOnly;
    sb::DeviceDefinition device;
    device.id = "runtime-config-device";
    device.elements.push_back({.id = "runtime-config-element", .writable = false});
    readOnly.devices.push_back(device);

    auto writable = readOnly;
    writable.devices.front().elements.front().writable = true;
    require(service::southbridge::config_redis::signature(readOnly) !=
                service::southbridge::config_redis::signature(writable),
            "runtime config signature ignored writable changes");

    const auto element = service::southbridge::config_redis::detail::element(
        {{"id", "runtime-config-element"}, {"writable", "1"}});
    require(element.writable, "runtime config did not deserialize writable state");
}

void testCommandValueDecimalParsing() {
    namespace commandValue = service::southbridge::command_value;
    require(commandValue::decimal("1.5", "value") == 1.5,
            "command decimal parser rejected a finite value");
    require(commandValue::decimal("-1.25e2", "value") == -125.0,
            "command decimal parser rejected scientific notation");

    for (const auto value : {"1.5x", "nan", "inf"}) {
        bool rejected = false;
        try {
            (void)commandValue::decimal(value, "value");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "command decimal parser accepted an invalid value");
    }
}

void testPacketLog() {
    namespace packetLog = service::common::packet_log;
    const auto directory = std::filesystem::temp_directory_path() / "iot-engine-packet-log-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);

    packetLog::Config config;
    config.directory = directory;
    config.level = packetLog::Level::Debug;
    packetLog::initialize(std::move(config));

    packetLog::Context context;
    context.workerIndex = 2;
    context.direction = "RX";
    context.operation = "transport";
    context.protocol = "Modbus";
    context.linkId = "link-test";
    context.connectionId = "connection-test";
    context.messageId = "message-test";
    context.sessionEpoch = 7;
    const std::array<std::uint8_t, 4> invalidBytes{0x00, 0xFF, 0x7E, 0x01};
    packetLog::write(packetLog::Level::Debug, "RX_BYTES", context, invalidBytes);
    packetLog::write(packetLog::Level::Warn, "TIMEOUT", context, {}, "modbus_response_timeout");
    packetLog::write(packetLog::Level::Info, "BROADCAST_START", context, {}, "targets=2");
    packetLog::shutdown();

    std::string content;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream input(entry.path());
        content.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    require(content.find("event=\"RX_BYTES\"") != std::string::npos &&
                content.find("hex=\"00 FF 7E 01\"") != std::string::npos,
            "packet log did not preserve unparsed binary bytes");
    require(content.find("event=\"TIMEOUT\"") != std::string::npos &&
                content.find("event=\"BROADCAST_START\"") != std::string::npos,
            "packet log did not persist timeout and broadcast events");
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main() {
    try {
        testCapabilities();
        testSl651();
        testSl651AllEncodingsAndFunctionCodes();
        testSl651MultiPacketImages();
        testModbus();
        testModbusTypesAndPriority();
        testModbusAllDataTypesAndByteOrders();
        testModbusAllFunctionCodes(true);
        testModbusAllFunctionCodes(false);
        testModbusRtuZeroAddress();
        testModbusDiscoveryAndOffline();
        testS7();
        testS7AllDataTypes();
        testWorkerTimer();
        testRuntimeConfigWritableContract();
        testCommandValueDecimalParsing();
        testPacketLog();
        std::cout << "southbridge protocol tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "southbridge protocol test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
