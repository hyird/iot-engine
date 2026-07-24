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

#include "service/common/packet-log.h"
#include "service/features/collector/command.h"
#include "service/features/collector/modbus.h"
#include "service/features/collector/engine.h"
#include "service/features/collector/s7.h"
#include "service/features/collector/sl651.h"
#include "service/features/collector/config.h"
#include "service/features/collector/timer.h"

namespace collector = service::collector;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

bool has(const std::vector<collector::ProtocolAction>& actions, collector::ProtocolActionKind kind) {
    return std::any_of(actions.begin(), actions.end(),
                       [kind](const auto& action) { return action.kind == kind; });
}

const collector::ProtocolAction& first(const std::vector<collector::ProtocolAction>& actions,
                                collector::ProtocolActionKind kind) {
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

std::vector<std::uint8_t> modbusReadResponseForRequest(std::span<const std::uint8_t> request,
                                                       bool tcp) {
    const auto functionOffset = tcp ? 7U : 1U;
    const auto quantityOffset = tcp ? 10U : 4U;
    require(request.size() >= quantityOffset + 2 && request[functionOffset] >= 1 &&
                request[functionOffset] <= 4,
            "Modbus poll response helper received an invalid read request");
    const auto function = request[functionOffset];
    const auto quantity = static_cast<std::size_t>(request[quantityOffset] << 8U) |
                          request[quantityOffset + 1];
    const auto byteCount = function <= 2 ? (quantity + 7U) / 8U : quantity * 2U;
    if (!tcp) {
        std::vector<std::uint8_t> response{request[0], function,
                                           static_cast<std::uint8_t>(byteCount)};
        response.resize(response.size() + byteCount, 0);
        return withModbusCrc(std::move(response));
    }
    const auto length = static_cast<std::uint16_t>(3 + byteCount);
    std::vector<std::uint8_t> response{request[0],
                                       request[1],
                                       0,
                                       0,
                                       static_cast<std::uint8_t>(length >> 8U),
                                       static_cast<std::uint8_t>(length),
                                       request[6],
                                       function,
                                       static_cast<std::uint8_t>(byteCount)};
    response.resize(response.size() + byteCount, 0);
    return response;
}

std::vector<collector::ProtocolAction>
drainInitialModbusPoll(collector::ProtocolEngine& engine, std::vector<collector::ProtocolAction> actions,
                       service::message::IngressPacket& packet, bool tcp) {
    while (true) {
        const auto outbound = std::find_if(actions.begin(), actions.end(), [](const auto& action) {
            return action.kind == collector::ProtocolActionKind::Send;
        });
        if (outbound == actions.end())
            return actions;
        packet.payload = modbusReadResponseForRequest(outbound->bytes, tcp);
        actions = engine.consume(packet);
    }
}

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

std::uint16_t s7Reference(std::span<const std::uint8_t> frame) {
    require(frame.size() >= 13, "S7 frame does not contain a PDU reference");
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame[11]) << 8U) | frame[12];
}

std::vector<std::uint8_t> s7CotpConfirm() {
    return {0x03, 0x00, 0x00, 0x0B, 0x06, 0xD0, 0x00, 0x01, 0x00, 0x06, 0x00};
}

std::vector<std::uint8_t> s7SetupResponse(std::uint16_t reference = 0) {
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
            0x08,
            0x00,
            0x00,
            0x00,
            0x00,
            0xF0,
            0x00,
            0x00,
            0x01,
            0x00,
            0x01,
            0x01,
            0xE0};
}

std::vector<std::uint8_t> s7ReadResponseForRequest(std::span<const std::uint8_t> request) {
    require(request.size() >= 31 && request[17] == 0x04,
            "S7 read response helper received an invalid request");
    const auto count = request[18];
    std::vector<std::uint8_t> data;
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = 19 + index * 12;
        require(offset + 12 <= request.size(), "S7 read request item is truncated");
        const auto amount = static_cast<std::size_t>(request[offset + 4] << 8U) |
                            request[offset + 5];
        const auto wordLength = request[offset + 3];
        const auto byteLength =
            amount * (wordLength == 0x1C || wordLength == 0x1D ? 2U : 1U);
        data.insert(data.end(), {0xFF, 0x04,
                                 static_cast<std::uint8_t>((byteLength * 8U) >> 8U),
                                 static_cast<std::uint8_t>(byteLength * 8U)});
        for (std::size_t byte = 0; byte < byteLength; ++byte)
            data.push_back(index == 0 && byte == 0 ? 0x12 : index == 0 && byte == 1 ? 0x34 : 0);
        if ((byteLength & 1U) != 0 && index + 1 < count)
            data.push_back(0);
    }
    const auto totalLength = static_cast<std::uint16_t>(21 + data.size());
    const auto dataLength = static_cast<std::uint16_t>(data.size());
    std::vector<std::uint8_t> response{0x03,
                                       0x00,
                                       static_cast<std::uint8_t>(totalLength >> 8U),
                                       static_cast<std::uint8_t>(totalLength),
                                       0x02,
                                       0xF0,
                                       0x80,
                                       0x32,
                                       0x03,
                                       0x00,
                                       0x00,
                                       request[11],
                                       request[12],
                                       0x00,
                                       0x02,
                                       static_cast<std::uint8_t>(dataLength >> 8U),
                                       static_cast<std::uint8_t>(dataLength),
                                       0x00,
                                       0x00,
                                       0x04,
                                       count};
    response.insert(response.end(), data.begin(), data.end());
    return response;
}

collector::ProtocolRuntimeRegistry runtimes() {
    collector::ProtocolRuntimeRegistry result;
    result.add(std::make_unique<collector::modbus::Runtime>());
    result.add(std::make_unique<collector::s7::Runtime>());
    result.add(std::make_unique<collector::sl651::Runtime>());
    return result;
}

void testCapabilities() {
    require(collector::DeviceDefinition{}.timezone == "+08:00", "device timezone must default to UTC+8");
    auto registry = runtimes();
    require(registry.require("SL651").capabilities().has(collector::ProtocolCapability::TcpServer),
            "SL651 must support TCP Server");
    require(!registry.require("SL651").capabilities().has(collector::ProtocolCapability::TcpClient),
            "SL651 must reject TCP Client");
    require(!registry.require("SL651").capabilities().has(collector::ProtocolCapability::Polling),
            "SL651 must not expose polling");
    require(registry.require("SL651").capabilities().has(collector::ProtocolCapability::Registration) &&
                registry.require("SL651").capabilities().has(collector::ProtocolCapability::Heartbeat),
            "SL651 registration or heartbeat capability missing");
    require(registry.require("Modbus").capabilities().has(collector::ProtocolCapability::Discovery),
            "Modbus discovery capability missing");
    require(registry.require("S7").capabilities().has(collector::ProtocolCapability::Polling),
            "S7 polling capability missing");
}

void testSl651() {
    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-link",
                              .name = "SL",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .ip = "127.0.0.1",
                              .port = 15001,
                              .status = "enabled"});
    collector::DeviceDefinition device;
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

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "sl-connection",
                            .linkId = "sl-link",
                            .remoteAddress = "127.0.0.1:10001",
                            .sessionEpoch = 1});
    const auto frame =
        slFrame(0x32, {0x00, 0x01, 0x24, 0x01, 0x02, 0x03, 0x04, 0x05, 0x39, 0x00, 0x12, 0x34});
    service::message::IngressPacket packet{.messageId = "sl-ingress",
                                          .linkId = "sl-link",
                                          .connectionId = "sl-connection",
                                          .remoteAddress = "127.0.0.1:10001",
                                          .occurredAtMs = 1000};
    packet.payload.assign(frame.begin(), frame.begin() + 5);
    require(engine.consume(packet).empty(), "partial SL651 header must not emit actions");
    packet.payload.assign(frame.begin() + 5, frame.end());
    const auto actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::BindDevice), "SL651 did not bind device code");
    const auto& parsed = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
    require(parsed.rawPayloads == std::vector<std::vector<std::uint8_t>>{frame},
            "SL651 raw frame was not preserved as a one-item array");
    require(parsed.valuesJson.find("12.34") != std::string::npos,
            "SL651 BCD element was not parsed");
    require(parsed.observedAtMs == 1704135845000,
            "SL651 device-local report time was not converted from UTC+8 to UTC");

    packet.payload = slFrame(0x4C);
    const auto responseActions = engine.consume(packet);
    const auto& responseParsed =
        first(responseActions, collector::ProtocolActionKind::PublishParsed).parsed;
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
    require(has(commandActions, collector::ProtocolActionKind::Send), "SL651 command was not sent");
    packet.payload = slFrame(0x32);
    commandActions = engine.consume(packet);
    require(!has(commandActions, collector::ProtocolActionKind::CompleteCommand),
            "SL651 unsolicited report completed a command");
    packet.payload = slFrame(0xE1, {});
    commandActions = engine.consume(packet);
    require(has(commandActions, collector::ProtocolActionKind::CompleteCommand),
            "SL651 ACK did not complete command");

    commandActions = engine.execute(
        "sl-connection", {.id = "sl-element-command",
                          .deviceId = "sl-device",
                          .deviceCode = "0000000001",
                          .kind = "command",
                          .elements = {{.elementId = "request-value", .value = "12.34"}}});
    const auto& generatedSl651 = first(commandActions, collector::ProtocolActionKind::Send).bytes;
    require(generatedSl651[2] == 0x00 && generatedSl651[6] == 0x01 && generatedSl651[7] == 0x01 &&
                generatedSl651[10] == 0x4C,
            "SL651 element command did not use the iot-manager default address header");
    const std::array<std::uint8_t, 4> encodedSl651Value{0x39, 0x00, 0x12, 0x34};
    require(std::search(generatedSl651.begin(), generatedSl651.end(), encodedSl651Value.begin(),
                        encodedSl651Value.end()) != generatedSl651.end(),
            "SL651 element command did not encode its guide and BCD value");
    packet.payload = slFrame(0xE1, {});
    commandActions = engine.consume(packet);
    require(has(commandActions, collector::ProtocolActionKind::CompleteCommand),
            "generated SL651 command did not complete on ACK");

    commandActions = engine.execute("sl-connection", {.id = "sl-negative-command",
                                                      .deviceId = "sl-device",
                                                      .deviceCode = "0000000001",
                                                      .kind = "control",
                                                      .payload = commandFrame});
    require(has(commandActions, collector::ProtocolActionKind::Send),
            "SL651 negative-ack test command was not sent");
    packet.payload = slFrame(0xE2, {});
    commandActions = engine.consume(packet);
    require(first(commandActions, collector::ProtocolActionKind::FailCommand).reason ==
                "sl651_negative_ack",
            "SL651 negative ACK did not fail with a precise reason");
}

void testSl651Registration() {
    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-registration-link",
                              .name = "SL registration",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .status = "enabled"});
    for (std::uint8_t index = 1; index <= 2; ++index) {
        collector::DeviceDefinition device;
        device.id = "sl-registration-device-" + std::to_string(index);
        device.code = index == 1 ? "0000000001" : "0000000002";
        device.linkId = "sl-registration-link";
        device.linkMode = "TCP Server";
        device.protocol = "SL651";
        device.registrationMode = "ASCII";
        device.registrationBytes = {'R', 'E', 'G'};
        device.heartbeatMode = "ASCII";
        device.heartbeatBytes = {'H', 'B'};
        snapshot.devices.push_back(std::move(device));
    }

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "sl-registration-connection",
                            .linkId = "sl-registration-link",
                            .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "sl-registration",
                                          .linkId = "sl-registration-link",
                                          .connectionId = "sl-registration-connection",
                                          .occurredAtMs = 1500,
                                          .payload = slFrame(0x32)};
    auto commandActions = engine.execute(
        "sl-registration-connection",
        {.id = "sl-before-registration",
         .deviceId = "sl-registration-device-1",
         .deviceCode = "0000000001",
         .kind = "command"});
    require(first(commandActions, collector::ProtocolActionKind::FailCommand).reason ==
                "sl651_device_offline",
            "SL651 accepted a command before the registration bound the device");
    auto actions = engine.consume(packet);
    require(actions.empty(), "SL651 parsed a frame before required registration");

    packet.payload = {'R', 'E', 'G'};
    actions = engine.consume(packet);
    require(std::count_if(actions.begin(), actions.end(), [](const auto& action) {
                return action.kind == collector::ProtocolActionKind::BindDevice;
            }) == 2,
            "SL651 standalone registration did not bind every device in the DTU group");
    actions = engine.consume(packet);
    require(!has(actions, collector::ProtocolActionKind::Close),
            "SL651 repeated registration closed its own DTU connection");
    packet.payload = {'H', 'B'};
    require(engine.consume(packet).empty(), "SL651 heartbeat leaked into frame parsing");

    (void)engine.connected({.connectionId = "sl-prefixed-registration",
                            .linkId = "sl-registration-link",
                            .sessionEpoch = 2});
    packet.connectionId = "sl-prefixed-registration";
    packet.payload = {'R', 'E', 'G'};
    const auto report = slFrame(0x32);
    packet.payload.insert(packet.payload.end(), report.begin(), report.end());
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::BindDevice) &&
                has(actions, collector::ProtocolActionKind::PublishParsed),
            "SL651 registration prefix did not preserve the report payload");
}

void testSl651AllEncodingsAndFunctionCodes() {
    collector::ElementDefinition element;
    element.encoding = "BCD";
    element.digits = 2;
    const std::array<std::uint8_t, 2> bcd{0x12, 0x34};
    require(collector::sl651::detail::elementValue(bcd, element) == "12.34", "SL651 BCD encoding failed");
    element.encoding = "TIME_YYMMDDHHMMSS";
    const std::array<std::uint8_t, 6> time{0x24, 0x01, 0x02, 0x03, 0x04, 0x05};
    require(collector::sl651::detail::elementValue(time, element) == "2024-01-02T03:04:05",
            "SL651 time encoding failed");
    const std::array<std::uint8_t, 3> binary{0x00, 0xAB, 0xFF};
    for (const auto encoding : {"HEX", "DICT"}) {
        element.encoding = encoding;
        require(collector::sl651::detail::elementValue(binary, element) == "00ABFF",
                "SL651 binary encoding failed");
    }
    element.encoding = "JPEG";
    require(collector::sl651::detail::elementValue(binary, element) == "INVALID_JPEG",
            "SL651 accepted an invalid JPEG");
    const std::array<std::uint8_t, 4> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    require(collector::sl651::detail::elementValue(jpeg, element) == "data:image/jpeg;base64,/9j/2Q==",
            "SL651 JPEG was not stored as a Base64 data URL");

    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-function-link",
                              .name = "SL function matrix",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .status = "enabled"});
    collector::DeviceDefinition device;
    device.id = "sl-function-device";
    device.code = "0000000001";
    device.linkId = "sl-function-link";
    device.protocol = "SL651";
    for (std::uint16_t function = 0; function <= 0xFF; ++function) {
        const auto code = collector::sl651::detail::hexByte(static_cast<std::uint8_t>(function));
        device.elements.push_back({.id = "fc-" + code,
                                   .name = "Function " + code,
                                   .functionCode = code,
                                   .guideHex = "3900",
                                   .encoding = "HEX",
                                   .length = 1});
    }
    snapshot.devices.push_back(std::move(device));

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "sl-function-connection",
                            .linkId = "sl-function-link",
                            .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "sl-function-frame",
                                          .linkId = "sl-function-link",
                                          .connectionId = "sl-function-connection",
                                          .occurredAtMs = 7000};
    for (std::uint16_t function = 0; function <= 0xFF; ++function) {
        const auto code = collector::sl651::detail::hexByte(static_cast<std::uint8_t>(function));
        packet.payload = slFrame(static_cast<std::uint8_t>(function),
                                 {0x39, 0x00, static_cast<std::uint8_t>(function)});
        const auto actions = engine.consume(packet);
        const auto& parsed = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
        require(parsed.valuesJson.find("fc-" + code) != std::string::npos,
                "SL651 configured function code was not routed");
    }
}

void testSl651MultiPacketImages() {
    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-image-link",
                              .name = "SL image",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .status = "enabled"});
    collector::DeviceDefinition device;
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

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected(
        {.connectionId = "sl-image-connection", .linkId = "sl-image-link", .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "sl-image-frame",
                                          .linkId = "sl-image-link",
                                          .connectionId = "sl-image-connection",
                                          .occurredAtMs = 8000};

    const auto firstFrame = slMultiFrame(0x36, 3, 1, {0x00, 0x01, 0x24, 0x01, 0x02});
    const auto secondFrame = slMultiFrame(0x36, 3, 2, {0x03, 0x04, 0x05, 0x39, 0x00, 0xFF});
    packet.payload = secondFrame;
    auto actions = engine.consume(packet);
    require(!has(actions, collector::ProtocolActionKind::PublishParsed),
            "SL651 published an incomplete image");
    const auto firstToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    packet.payload = firstFrame;
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CancelDeadline),
            "SL651 did not cancel the previous image idle deadline");
    const auto secondToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    require(secondToken != firstToken, "SL651 did not refresh the image idle deadline");
    require(engine.deadline("sl-image-connection", firstToken).empty(),
            "a stale SL651 image deadline affected the refreshed assembly");

    const auto thirdFrame = slMultiFrame(0x36, 3, 3, {0xD8, 0xFF, 0xD9});
    packet.payload = thirdFrame;
    actions = engine.consume(packet);
    const auto& parsed = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
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
    const auto streamFields = service::message::parsedFields(parsed);
    service::message::StreamMessage streamMessage{.id = "1-0", .fields = streamFields};
    const auto roundTrip = service::message::parsedFrom(streamMessage);
    require(roundTrip.rawPayloads == expectedRaw &&
                streamMessage.get("raw_payload_hex") ==
                    service::message::rawPayloadsJson(expectedRaw),
            "parsed Redis message did not preserve the ordered HEX payload array");

    const auto oldFirst = slMultiFrame(0x36, 2, 1, {0x00, 0x01, 0x24, 0x01});
    packet.payload = oldFirst;
    actions = engine.consume(packet);
    const auto oldToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    const auto newFirst = slMultiFrame(
        0x36, 2, 1, {0x00, 0x01, 0x24, 0x01, 0x02, 0x03, 0x04, 0x05, 0x39, 0x00, 0xFF, 0xD8});
    packet.payload = newFirst;
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::CancelDeadline).deadlineToken == oldToken,
            "a new SL651 image did not replace the previous partial image");
    require(engine.deadline("sl-image-connection", oldToken).empty(),
            "the replaced SL651 image deadline affected the new image");
    const auto newLast = slMultiFrame(0x36, 2, 2, {0xFF, 0xD9});
    packet.payload = newLast;
    actions = engine.consume(packet);
    const auto& replacement = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
    const std::vector<std::vector<std::uint8_t>> replacementRaw{newFirst, newLast};
    require(replacement.rawPayloads == replacementRaw,
            "the replaced SL651 image leaked old raw packets into storage");
}

void testModbus() {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = "modbus-link",
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
    collector::DeviceDefinition device;
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

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    auto connected = engine.connected({.connectionId = "modbus-connection",
                                       .linkId = "modbus-link",
                                       .remoteAddress = "127.0.0.1:15002",
                                       .targetId = "target-1",
                                       .sessionEpoch = 1});
    require(has(connected, collector::ProtocolActionKind::BindDevice),
            "Modbus client target was not bound");
    require(has(connected, collector::ProtocolActionKind::Send),
            "Modbus did not trigger the first poll immediately after binding");
    service::message::IngressPacket packet{.messageId = "modbus-ingress",
                                          .linkId = "modbus-link",
                                          .connectionId = "modbus-connection",
                                          .remoteAddress = "127.0.0.1:15002",
                                          .occurredAtMs = 2000};
    connected = drainInitialModbusPoll(engine, std::move(connected), packet, true);
    const auto pollToken = first(connected, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    auto actions = engine.execute("modbus-connection", {.id = "modbus-read",
                                                        .deviceId = "modbus-device",
                                                         .deviceCode = "MODBUS-1",
                                                         .kind = "read",
                                                         .payload = modbusRead(1)});
    require(has(actions, collector::ProtocolActionKind::Send), "Modbus read was not dispatched");
    packet.payload = modbusReadResponse(1);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::PublishParsed), "Modbus response was not parsed");
    require(has(actions, collector::ProtocolActionKind::CompleteCommand), "Modbus read did not complete");

    actions = engine.execute("modbus-connection", {.id = "invalid-write",
                                                   .deviceId = "modbus-device",
                                                   .deviceCode = "MODBUS-1",
                                                   .kind = "write",
                                                   .payload = modbusWrite(2)});
    require(first(actions, collector::ProtocolActionKind::FailCommand).reason ==
                "modbus_write_readback_required",
            "Modbus accepted a write without readback");

    actions = engine.execute("modbus-connection", {.id = "verified-write",
                                                   .deviceId = "modbus-device",
                                                   .deviceCode = "MODBUS-1",
                                                   .kind = "write",
                                                   .payload = modbusWrite(3),
                                                   .readbackPayload = modbusRead(4),
                                                   .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusWrite(3),
            "Modbus write was not sent first");
    packet.payload = modbusWrite(3);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(4),
            "Modbus write response did not trigger readback");
    packet.payload = modbusReadResponse(4);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus verified write did not complete");

    actions = engine.execute("modbus-connection",
                             {.id = "element-write",
                              .deviceId = "modbus-device",
                              .deviceCode = "MODBUS-1",
                              .kind = "command",
                              .elements = {{.elementId = "holding-0", .value = "4660"}}});
    const auto generatedWrite = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedWrite.size() == 12 && generatedWrite[7] == 6 &&
                generatedWrite[10] == 0x12 && generatedWrite[11] == 0x34,
            "Modbus element command did not compile FC06");
    packet.payload = generatedWrite;
    actions = engine.consume(packet);
    const auto generatedReadback = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedReadback.size() == 12 && generatedReadback[7] == 3,
            "Modbus element command did not compile FC03 readback");
    const auto readbackTransaction = static_cast<std::uint16_t>(generatedReadback[0] << 8U) |
                                     generatedReadback[1];
    packet.payload = modbusReadResponse(readbackTransaction);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus element command did not complete after readback");

    actions = engine.deadline("modbus-connection", pollToken);
    require(has(actions, collector::ProtocolActionKind::Send),
            "Modbus periodic poll was not generated by its session");
    const auto& poll = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(poll.size() == 12 && poll[7] == 3 && poll[10] == 0 && poll[11] == 3,
            "Modbus register mergeGap did not combine one read range");
}

void testModbusTypesAndPriority() {
    collector::ElementDefinition element;
    element.dataType = "UINT64";
    element.byteOrder = "BIG_ENDIAN";
    const std::array<std::uint8_t, 8> maximum{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    require(collector::modbus::detail::numericJson(maximum, element) == "18446744073709551615",
            "Modbus UINT64 lost integer precision");
    element.dataType = "UINT32";
    element.byteOrder = "BIG_ENDIAN_BYTE_SWAP";
    const std::array<std::uint8_t, 4> bigSwap{0x34, 0x12, 0x78, 0x56};
    require(collector::modbus::detail::numericJson(bigSwap, element) == "305419896",
            "Modbus BIG_ENDIAN_BYTE_SWAP was decoded incorrectly");
    element.byteOrder = "LITTLE_ENDIAN_BYTE_SWAP";
    const std::array<std::uint8_t, 4> littleSwap{0x56, 0x78, 0x12, 0x34};
    require(collector::modbus::detail::numericJson(littleSwap, element) == "305419896",
            "Modbus LITTLE_ENDIAN_BYTE_SWAP was decoded incorrectly");

    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = "priority-link",
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
    collector::DeviceDefinition device;
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

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    auto connected = engine.connected({.connectionId = "priority-connection",
                                       .linkId = link.id,
                                       .targetId = "priority-target",
                                       .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "initial-poll-response",
                                          .linkId = link.id,
                                          .connectionId = "priority-connection",
                                          .occurredAtMs = 4999};
    (void)drainInitialModbusPoll(engine, std::move(connected), packet, true);
    auto actions = engine.execute("priority-connection", {.id = "active-read",
                                                          .deviceId = device.id,
                                                          .deviceCode = device.code,
                                                          .kind = "read",
                                                          .payload = modbusRead(10, 1)});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(10, 1),
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
    packet.messageId = "active-response";
    packet.occurredAtMs = 5000;
    packet.payload = modbusReadResponse(10, 1, {0x01});
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::PublishParsed)
                    .parsed.valuesJson.find("\"value\":1") != std::string::npos,
            "Modbus coil response was not parsed bitwise");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusWrite(12),
            "Modbus high-priority write did not jump ahead of a normal read");
    packet.payload = modbusWrite(12);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(13),
            "Modbus high-priority write did not enter atomic readback");
    packet.payload = modbusReadResponse(13);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(11, 1),
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
        collector::ElementDefinition element;
        element.dataType = current.type;
        for (const auto order : orders) {
            element.byteOrder = order;
            const auto wire = collector::modbus::detail::orderedBytes(current.canonical, order);
            const auto decoded = collector::modbus::detail::numericJson(wire, element);
            require(decoded && *decoded == current.expected,
                    "Modbus data type or byte order matrix failed");
        }
    }

    collector::ElementDefinition scaled;
    scaled.dataType = "UINT16";
    scaled.byteOrder = "BIG_ENDIAN";
    scaled.scale = 0.1;
    scaled.decimals = 1;
    const std::array<std::uint8_t, 2> raw{0x04, 0xD2};
    require(collector::modbus::detail::numericJson(raw, scaled) == "123.4",
            "Modbus scale and decimals were not applied");
}

void testModbusAllFunctionCodes(bool tcp) {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = tcp ? "modbus-tcp-matrix" : "modbus-rtu-matrix",
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
    collector::DeviceDefinition device;
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

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    service::message::IngressPacket packet{.messageId = "modbus-matrix-response",
                                          .linkId = link.id,
                                          .connectionId = "modbus-matrix-connection",
                                          .occurredAtMs = 6000};
    auto connected = engine.connected({.connectionId = "modbus-matrix-connection",
                                       .linkId = link.id,
                                       .targetId = "matrix-target",
                                       .sessionEpoch = 1});
    (void)drainInitialModbusPoll(engine, std::move(connected), packet, tcp);
    std::uint16_t transaction = 100;
    for (const auto function : std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 15, 16}) {
        const auto request = modbusRequest(tcp, transaction, function);
        collector::ProtocolCommand command{
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
        require(first(actions, collector::ProtocolActionKind::Send).bytes == request,
                "Modbus function code request was not dispatched");
        packet.payload = modbusResponse(tcp, transaction - (write ? 1 : 0), function);
        actions = engine.consume(packet);
        if (write) {
            require(first(actions, collector::ProtocolActionKind::Send).bytes ==
                        modbusRequest(tcp, readTransaction, readFunction),
                    "Modbus write function did not trigger readback");
            packet.payload = modbusResponse(tcp, readTransaction, readFunction);
            actions = engine.consume(packet);
        }
        require(has(actions, collector::ProtocolActionKind::CompleteCommand),
                "Modbus function code did not complete");
        ++transaction;
    }
}

void testModbusRtuZeroAddress() {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = "modbus-rtu-link",
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
    collector::DeviceDefinition device;
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

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    auto connected = engine.connected({.connectionId = "modbus-rtu-connection",
                                       .linkId = link.id,
                                       .targetId = "rtu-target",
                                       .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "rtu-initial-poll-response",
                                          .linkId = link.id,
                                          .connectionId = "modbus-rtu-connection",
                                          .occurredAtMs = 2999};
    (void)drainInitialModbusPoll(engine, std::move(connected), packet, false);
    auto actions = engine.execute("modbus-rtu-connection", {.id = "rtu-read",
                                                            .deviceId = device.id,
                                                            .deviceCode = device.code,
                                                            .kind = "read",
                                                            .payload = modbusRtuRead()});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRtuRead(),
            "zero-address Modbus RTU read was mistaken for TCP");
    packet.messageId = "rtu-read-response";
    packet.occurredAtMs = 3000;
    packet.payload = modbusRtuReadResponse();
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "zero-address Modbus RTU response did not complete");

    actions = engine.execute("modbus-rtu-connection", {.id = "rtu-write",
                                                       .deviceId = device.id,
                                                       .deviceCode = device.code,
                                                       .kind = "write",
                                                       .payload = modbusRtuWrite(),
                                                       .readbackPayload = modbusRtuRead(),
                                                       .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRtuWrite(),
            "zero-address Modbus RTU write was mistaken for TCP");
    packet.messageId = "rtu-write-response";
    packet.payload = modbusRtuWrite();
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRtuRead(),
            "Modbus RTU write did not trigger readback");
    packet.messageId = "rtu-readback-response";
    packet.payload = modbusRtuReadResponse();
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus RTU write readback did not complete");
}

void testModbusDiscoveryAndOffline() {
    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "modbus-server",
                              .name = "Modbus Server",
                              .mode = "TCP Server",
                              .protocol = "Modbus",
                              .status = "enabled"});
    for (std::uint8_t unit = 1; unit <= 2; ++unit) {
        collector::DeviceDefinition device;
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
    collector::ProtocolEngine engine(runtimes());
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
        first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    service::message::IngressPacket packet{.messageId = "discovery-response-1",
                                          .linkId = "modbus-server",
                                          .connectionId = "discovery-connection",
                                          .occurredAtMs = 4000,
                                          .payload = modbusReadResponse(11)};
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::BindDevice).deviceCode == "DISCOVERY1",
            "Modbus discovery did not register the first response");
    packet.messageId = "discovery-response-2";
    packet.payload[6] = 2;
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::BindDevice).deviceCode == "DISCOVERY2",
            "Modbus discovery did not register the second response");
    actions = engine.deadline("discovery-connection", discoveryToken);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus discovery window did not complete");

    collector::RuntimeSnapshot registrationSnapshot;
    registrationSnapshot.links = snapshot.links;
    auto registeredDevice = snapshot.devices.front();
    registeredDevice.registrationMode = "ASCII";
    registeredDevice.registrationBytes = {'R', 'E', 'G'};
    registrationSnapshot.devices.push_back(registeredDevice);
    auto conflictingDevice = registeredDevice;
    conflictingDevice.id = "registration-conflict";
    conflictingDevice.code = "REGISTRATION-CONFLICT";
    conflictingDevice.slaveId = 2;
    conflictingDevice.registrationBytes = {'B', 'A', 'D'};
    registrationSnapshot.devices.push_back(conflictingDevice);
    engine.reload(registrationSnapshot);
    (void)engine.connected(
        {.connectionId = "registration-connection", .linkId = "modbus-server", .sessionEpoch = 2});
    packet.connectionId = "registration-connection";
    packet.payload = {'R', 'E', 'G'};
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "Modbus standalone registration packet did not bind the device");
    require(!has(actions, collector::ProtocolActionKind::ScheduleDeadline),
            "Modbus incorrectly applied a device-online timeout to the persistent DTU socket");
    actions = engine.consume(packet);
    require(!has(actions, collector::ProtocolActionKind::Close),
            "Modbus repeated registration packet closed its own DTU connection");
    packet.payload = {'B', 'A', 'D'};
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Close).reason ==
                "modbus_registration_conflict",
            "Modbus conflicting registration was not rejected");

    (void)engine.connected(
        {.connectionId = "prefixed-registration", .linkId = "modbus-server", .sessionEpoch = 3});
    packet.connectionId = "prefixed-registration";
    packet.payload = {'R', 'E', 'G'};
    const auto prefixedResponse = modbusReadResponse(11);
    packet.payload.insert(packet.payload.end(), prefixedResponse.begin(), prefixedResponse.end());
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::BindDevice) &&
                !has(actions, collector::ProtocolActionKind::Close),
            "Modbus registration prefix did not preserve the payload after binding");
}

void testS7() {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{
        .id = "s7-link", .name = "S7", .mode = "TCP Client", .protocol = "S7", .status = "enabled"};
    link.targets.push_back(
        {.id = "target-1", .name = "PLC", .ip = "127.0.0.1", .port = 102, .status = "enabled"});
    snapshot.links.push_back(link);
    collector::DeviceDefinition device;
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
                               .size = 1,
                               .writable = true});
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

    const std::vector<std::uint8_t> expectedCotp{0x03, 0x00, 0x00, 0x16, 0x11, 0xE0,
                                                 0x00, 0x00, 0x00, 0x01, 0x00, 0xC0,
                                                 0x01, 0x0A, 0xC1, 0x02, 0x01, 0x00,
                                                 0xC2, 0x02, 0x01, 0x01};
    const std::vector<std::uint8_t> expectedSetup{0x03, 0x00, 0x00, 0x19, 0x02, 0xF0, 0x80,
                                                  0x32, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                  0x08, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x01,
                                                  0x00, 0x01, 0x01, 0xE0};
    const std::vector<std::uint8_t> expectedDisconnect{0x03, 0x00, 0x00, 0x0B, 0x06, 0x80,
                                                       0x00, 0x06, 0x00, 0x01, 0x00};

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    service::message::IngressPacket packet{.messageId = "s7-ingress",
                                          .linkId = "s7-link",
                                          .connectionId = "s7-connection",
                                          .remoteAddress = "127.0.0.1:102",
                                          .occurredAtMs = 3000};
    const auto finishHandshake = [&]() {
        packet.payload = s7CotpConfirm();
        auto handshake = engine.consume(packet);
        require(first(handshake, collector::ProtocolActionKind::Send).bytes == expectedSetup,
                "S7 Setup Communication packet does not match iot-manager");
        require(!has(handshake, collector::ProtocolActionKind::ScheduleDeadline),
                "S7 handshake timeout was incorrectly restarted after ISO-CC");
        packet.payload = s7SetupResponse();
        return engine.consume(packet);
    };

    auto actions = engine.connected({.connectionId = "s7-connection",
                                     .linkId = "s7-link",
                                     .remoteAddress = "127.0.0.1:102",
                                     .targetId = "target-1",
                                     .sessionEpoch = 1});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 ISO-CR packet does not match iot-manager");
    require(first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineAfter ==
                std::chrono::seconds(5),
            "S7 handshake timeout does not match iot-manager");
    require(!has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 device became online before protocol negotiation");

    actions = finishHandshake();
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 device was not bound after protocol negotiation");
    const auto immediatePoll = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(immediatePoll[17] == 0x04,
            "S7 did not trigger the first poll immediately after registration");
    require(immediatePoll.size() == 19 + 7 * 12 && immediatePoll[18] == 7,
            "S7 initial poll did not batch elements within the negotiated PDU");
    packet.payload = s7ReadResponseForRequest(immediatePoll);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::PublishParsed),
            "S7 immediate poll response was not parsed");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedDisconnect,
            "S7 did not send the iot-manager ISO-DR packet after polling");
    const auto pollToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    actions = engine.execute("s7-connection", {.id = "s7-read",
                                               .deviceId = "s7-device",
                                               .deviceCode = "S7-1",
                                               .kind = "read",
                                               .payload = s7ReadRequest(99)});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 active read did not open a fresh PLC session");
    actions = finishHandshake();
    require(first(actions, collector::ProtocolActionKind::Send).bytes == s7ReadRequest(1),
            "S7 active read PDU sequence does not match iot-manager");
    packet.payload = s7ReadResponse(1);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::PublishParsed), "S7 response was not parsed");
    require(has(actions, collector::ProtocolActionKind::CompleteCommand), "S7 read did not complete");
    require(first(actions, collector::ProtocolActionKind::PublishParsed).parsed.valuesJson.find("4660") !=
                std::string::npos,
            "S7 DB value was not decoded");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedDisconnect,
            "S7 read did not close only the PLC session");

    actions = engine.execute("s7-connection", {.id = "s7-write",
                                               .deviceId = "s7-device",
                                               .deviceCode = "S7-1",
                                               .kind = "write",
                                               .payload = s7WriteRequest(77),
                                               .readbackPayload = s7ReadRequest(78),
                                               .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 write did not open a fresh PLC session");
    actions = finishHandshake();
    require(first(actions, collector::ProtocolActionKind::Send).bytes == s7WriteRequest(1),
            "S7 Write Var PDU sequence does not match iot-manager");
    packet.payload = s7WriteResponse(1);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == s7ReadRequest(2),
            "S7 Write Var did not trigger same-session Read Var verification");
    packet.payload = s7ReadResponse(2);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "S7 Write Var readback did not complete");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedDisconnect,
            "S7 write verification did not finish with ISO-DR");

    actions =
        engine.execute("s7-connection", {.id = "s7-element-write",
                                         .deviceId = "s7-device",
                                         .deviceCode = "S7-1",
                                         .kind = "command",
                                         .elements = {{.elementId = "db1-word", .value = "4660"}}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 element write did not open a fresh PLC session");
    actions = finishHandshake();
    const auto generatedWrite = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedWrite.size() == 37 && generatedWrite[17] == 0x05 &&
                generatedWrite[35] == 0x12 && generatedWrite[36] == 0x34 &&
                s7Reference(generatedWrite) == 1,
            "S7 element command did not compile Write Var");
    packet.payload = s7WriteResponse(1);
    actions = engine.consume(packet);
    const auto generatedReadback = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedReadback[17] == 0x04 && s7Reference(generatedReadback) == 2,
            "S7 element command did not compile Read Var verification");
    packet.payload = s7ReadResponse(2);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "S7 element command did not complete after readback");

    actions = engine.execute(
        "s7-connection", {.id = "s7-bool-write",
                          .deviceId = "s7-device",
                          .deviceCode = "S7-1",
                          .kind = "command",
                          .elements = {{.elementId = "output-bit", .value = "1"}}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 BOOL write did not open a fresh PLC session");
    actions = finishHandshake();
    const auto prepareRead = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(prepareRead[17] == 0x04 && prepareRead[22] == 0x02 &&
                s7Reference(prepareRead) == 1,
            "S7 BOOL write did not read the containing byte first");
    packet.payload = s7ReadResponseForRequest(prepareRead);
    actions = engine.consume(packet);
    const auto boolWrite = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(boolWrite[17] == 0x05 && boolWrite[22] == 0x02 && boolWrite.back() == 0x16 &&
                s7Reference(boolWrite) == 2,
            "S7 BOOL write did not preserve adjacent bits during read-modify-write");
    packet.payload = s7WriteResponse(2);
    actions = engine.consume(packet);
    const auto boolReadback = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(boolReadback[17] == 0x04 && boolReadback[22] == 0x02 &&
                s7Reference(boolReadback) == 3,
            "S7 BOOL write did not start same-session byte readback");
    packet.payload = s7ReadResponseForRequest(boolReadback);
    packet.payload.back() = 0x16;
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "S7 BOOL write did not complete after bit-level readback verification");

    actions = engine.deadline("s7-connection", pollToken);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 periodic poll did not open a fresh PLC session");
    actions = finishHandshake();
    const auto& batched = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(batched.size() == 19 + 7 * 12 && batched[18] == 7,
            "S7 periodic poll did not batch elements within the negotiated PDU");
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

    collector::ElementDefinition decoded;
    decoded.dataType = "LREAL";
    const std::array<std::uint8_t, 8> one{0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    require(collector::s7::detail::decodeJson(one, decoded) == "1", "S7 LREAL was not decoded");
    decoded.dataType = "STRING";
    const std::array<std::uint8_t, 4> text{'A', 'B', 'C', 0x00};
    require(collector::s7::detail::decodeJson(text, decoded) == "\"ABC\"", "S7 STRING was not decoded");

    collector::RuntimeSnapshot serverSnapshot;
    serverSnapshot.links.push_back({.id = "s7-server",
                                    .name = "S7 Server",
                                    .mode = "TCP Server",
                                    .protocol = "S7",
                                    .status = "enabled"});
    auto serverDevice = device;
    serverDevice.id = "s7-server-device";
    serverDevice.code = "860406088591522";
    serverDevice.linkId = "s7-server";
    serverDevice.linkMode = "TCP Server";
    serverDevice.targetId.clear();
    serverDevice.registrationMode = "HEX";
    serverDevice.registrationBytes = {0x08, 0x60, 0x40, 0x60, 0x88, 0x59, 0x15, 0x22};
    serverSnapshot.devices.push_back(serverDevice);
    auto conflictingDevice = serverDevice;
    conflictingDevice.id = "s7-server-device-2";
    conflictingDevice.code = "860406088591523";
    conflictingDevice.registrationBytes.back() = 0x23;
    serverSnapshot.devices.push_back(conflictingDevice);

    collector::ProtocolEngine serverEngine(runtimes());
    serverEngine.reload(serverSnapshot);
    (void)serverEngine.connected({.connectionId = "s7-server-connection",
                                  .linkId = "s7-server",
                                  .sessionEpoch = 1});
    service::message::IngressPacket serverPacket{.messageId = "s7-registration",
                                                .linkId = "s7-server",
                                                .connectionId = "s7-server-connection",
                                                .occurredAtMs = 4000,
                                                .payload = serverDevice.registrationBytes};
    actions = serverEngine.consume(serverPacket);
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 standalone registration packet did not bind the device");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 registration did not start the PLC session");
    const auto handshakeDeadline = std::find_if(
        actions.begin(), actions.end(), [](const auto& action) {
            return action.kind == collector::ProtocolActionKind::ScheduleDeadline &&
                   action.deadlineAfter == std::chrono::seconds(5);
        });
    require(handshakeDeadline != actions.end(), "S7 registration omitted handshake timeout");
    const auto handshakeDeadlineToken = handshakeDeadline->deadlineToken;
    actions = serverEngine.consume(serverPacket);
    require(!has(actions, collector::ProtocolActionKind::Close),
            "S7 repeated registration packet closed its own DTU connection");
    serverPacket.payload = conflictingDevice.registrationBytes;
    actions = serverEngine.consume(serverPacket);
    require(first(actions, collector::ProtocolActionKind::Close).reason == "s7_registration_conflict",
            "S7 conflicting registration was not rejected");
    actions = serverEngine.deadline("s7-server-connection", handshakeDeadlineToken);
    require(!has(actions, collector::ProtocolActionKind::Close),
            "S7 handshake failure incorrectly closed the persistent DTU socket");

    (void)serverEngine.connected({.connectionId = "s7-prefixed-registration",
                                  .linkId = "s7-server",
                                  .sessionEpoch = 2});
    serverPacket.connectionId = "s7-prefixed-registration";
    serverPacket.payload = serverDevice.registrationBytes;
    const auto cotpConfirm = s7CotpConfirm();
    serverPacket.payload.insert(serverPacket.payload.end(), cotpConfirm.begin(), cotpConfirm.end());
    actions = serverEngine.consume(serverPacket);
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 registration prefix did not bind the device");
    require(std::count_if(actions.begin(), actions.end(), [](const auto& action) {
                return action.kind == collector::ProtocolActionKind::Send;
            }) == 2,
            "S7 registration prefix payload was not preserved for protocol parsing");
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
        collector::ElementDefinition element;
        element.dataType = current.type;
        element.startBit = current.startBit;
        const auto decoded = collector::s7::detail::decodeJson(current.bytes, element);
        require(decoded && *decoded == current.expected, "S7 data type matrix failed");
    }
    const std::string invalidUtf8{static_cast<char>(0xC0), '"'};
    require(collector::s7::detail::jsonEscape(invalidUtf8) == "\\u00C0\\\"",
            "S7 invalid string bytes did not produce valid JSON escapes");
    require(collector::s7::detail::jsonEscape("中文") == "中文", "S7 valid UTF-8 text was not preserved");
}

void testWorkerTimer() {
    asio::io_context io;
    collector::Timer scheduler(io);
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

void testRuntimeWritableContract() {
    collector::RuntimeSnapshot readOnly;
    collector::DeviceDefinition device;
    device.id = "runtime-config-device";
    device.elements.push_back({.id = "runtime-config-element", .writable = false});
    readOnly.devices.push_back(device);

    auto writable = readOnly;
    writable.devices.front().elements.front().writable = true;
    require(service::collector::config::signature(readOnly) !=
                service::collector::config::signature(writable),
            "runtime signature ignored writable changes");

    const auto element = service::collector::config::detail::element(
        {{"id", "runtime-config-element"}, {"writable", "1"}});
    require(element.writable, "runtime did not deserialize writable state");
}

void testCommandValueDecimalParsing() {
    namespace command = service::collector::command;
    require(command::decimal("1.5", "value") == 1.5,
            "command decimal parser rejected a finite value");
    require(command::decimal("-1.25e2", "value") == -125.0,
            "command decimal parser rejected scientific notation");

    for (const auto value : {"1.5x", "nan", "inf"}) {
        bool rejected = false;
        try {
            (void)command::decimal(value, "value");
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

void testEdgeParsedMessageContract() {
    service::message::ParsedDeviceMessage parsed;
    parsed.messageId = "019f91c9-4087-7e6c-88c0-c431b0dc15d8";
    parsed.causationId = parsed.messageId;
    parsed.deviceId = "019f91bf-6f83-7491-8a53-cd4fde034b72";
    parsed.deviceCode = "PCS7";
    parsed.protocol = "S7";
    parsed.connectionId = "019f8c99-913c-7a0c-b6ad-c43bb9b12764";
    parsed.occurredAtMs = 1784856700000;
    parsed.observedAtMs = 1784856700000;
    parsed.source = "edge";
    parsed.valuesJson = R"json({"values":{"VW0":{"name":"VW0","value":1,"unit":""}}})json";

    service::message::StreamMessage streamMessage{.id = "1-0",
                                                  .fields = service::message::parsedFields(parsed)};
    const auto roundTrip = service::message::parsedFrom(streamMessage);
    require(roundTrip.linkId.empty(), "edge parsed message required a link id");
    require(roundTrip.rawPayloads.empty(), "edge parsed message changed empty raw payloads");
    require(roundTrip.deviceCode == "PCS7" && roundTrip.source == "edge",
            "edge parsed message did not round-trip required fields");
}

} // namespace

int main() {
    try {
        const auto run = [](std::string_view name, auto&& test) {
            std::cerr << "[ RUN      ] " << name << '\n';
            test();
        };
        run("capabilities", testCapabilities);
        run("sl651", testSl651);
        run("sl651 registration", testSl651Registration);
        run("sl651 encodings", testSl651AllEncodingsAndFunctionCodes);
        run("sl651 multi-packet images", testSl651MultiPacketImages);
        run("modbus", testModbus);
        run("modbus types and priority", testModbusTypesAndPriority);
        run("modbus data types", testModbusAllDataTypesAndByteOrders);
        run("modbus TCP functions", [] { testModbusAllFunctionCodes(true); });
        run("modbus RTU functions", [] { testModbusAllFunctionCodes(false); });
        run("modbus RTU zero address", testModbusRtuZeroAddress);
        run("modbus discovery and registration", testModbusDiscoveryAndOffline);
        run("s7", testS7);
        run("s7 data types", testS7AllDataTypes);
        run("worker timer", testWorkerTimer);
        run("runtime writable contract", testRuntimeWritableContract);
        run("command value decimal parsing", testCommandValueDecimalParsing);
        run("edge parsed message contract", testEdgeParsedMessageContract);
        run("packet log", testPacketLog);
        std::cout << "collector protocol tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "collector protocol test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
