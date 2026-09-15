#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "service/features/collector/dlt645/dlt645.protocol.h"
#include "service/features/collector/fins/fins.protocol.h"
#include "service/features/collector/mc/mc.protocol.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename Function> void rejects(Function&& function, const char* message) {
    try { function(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

void testMcFrames() {
    using namespace service::collector::mc;
    Connection connection;
    Address address{.deviceCode = 0xa8, .number = 100, .count = 2};
    const std::vector<std::uint8_t> request{0x50,0,0,0xff,0xff,3,0,12,0,16,0,1,4,0,0,100,0,0,0xa8,2,0};
    require(FrameCodec::read(connection, address, 1) == request, "MC 3E read golden frame mismatch");
    const std::vector<std::uint8_t> response{0xd0,0,0,0xff,0xff,3,0,6,0,0,0,0x34,0x12,0x78,0x56};
    const auto decoded = FrameCodec::response(connection, address, 1, false, response);
    require(decoded.endCode == 0 && decoded.data == std::vector<std::uint8_t>({0x34,0x12,0x78,0x56}), "MC words changed byte order");
    for (std::size_t i = 0; i < 9; ++i)
        require(!FrameCodec::frameLength(std::span(response).first(i), connection.frame), "MC partial header treated as complete");
    auto corrupt = response; corrupt[6] = 1;
    rejects([&] { FrameCodec::response(connection, address, 1, false, corrupt); }, "MC accepted wrong station");
    corrupt = response; corrupt[7] = 1;
    rejects([&] { FrameCodec::frameLength(corrupt, connection.frame); }, "MC accepted impossible length");
    corrupt = response; corrupt[9] = 0x59; corrupt[10] = 0xc0;
    require(FrameCodec::response(connection, address, 1, false, corrupt).endCode == 0xc059, "MC lost device error code");
    connection.frame = FrameFormat::Binary4E;
    const auto request4 = FrameCodec::read(connection, address, 0x1234);
    require(request4[0] == 0x54 && request4[2] == 0x34 && request4[3] == 0x12 && request4.size() == request.size() + 4, "MC 4E serial encoding invalid");
    auto response4 = response;
    response4[0] = 0xd4; response4.insert(response4.begin() + 2, {0x34,0x12,0,0});
    require(FrameCodec::response(connection, address, 0x1234, false, response4).data == decoded.data, "MC 4E response invalid");
    rejects([&] { FrameCodec::response(connection, address, 0x1235, false, response4); }, "MC accepted late serial");
    address = {.deviceCode = 0x90, .number = 0, .count = 3, .bitAccess = true};
    const std::array<std::uint8_t, 3> bits{1,0,1};
    const auto write = FrameCodec::write(connection, address, 7, bits);
    require(write[write.size() - 2] == 0x10 && write.back() == 0x10, "MC bit packing invalid");
}

void testFinsFrames() {
    using namespace service::collector::fins;
    const std::vector<std::uint8_t> handshake{'F','I','N','S',0,0,0,12,0,0,0,0,0,0,0,0,0,0,0,0};
    require(FrameCodec::nodeRequest(0) == handshake, "FINS node request golden mismatch");
    const std::vector<std::uint8_t> handshakeReply{'F','I','N','S',0,0,0,16,0,0,0,1,0,0,0,0,0,0,0,10,0,0,0,20};
    auto connection = FrameCodec::nodeResponse({}, handshakeReply);
    require(connection.sourceNode == 10 && connection.destinationNode == 20, "FINS node negotiation mismatch");
    Address address{.memoryArea = 0x82, .word = 100, .count = 2};
    const auto request = FrameCodec::read(connection, address, 7);
    const std::vector<std::uint8_t> expected{'F','I','N','S',0,0,0,26,0,0,0,2,0,0,0,0,0x80,0,2,0,20,0,0,10,0,7,1,1,0x82,0,100,0,0,2};
    require(request == expected, "FINS memory read golden mismatch");
    std::vector<std::uint8_t> response{'F','I','N','S',0,0,0,26,0,0,0,2,0,0,0,0,0xc0,0,2,0,10,0,0,20,0,7,1,1,0,0,0x12,0x34,0x56,0x78};
    require(FrameCodec::response(connection, address, 7, false, response).data == std::vector<std::uint8_t>({0x12,0x34,0x56,0x78}), "FINS read word order mismatch");
    rejects([&] { FrameCodec::response(connection, address, 8, false, response); }, "FINS accepted stale SID");
    response[23] = 21;
    rejects([&] { FrameCodec::response(connection, address, 7, false, response); }, "FINS accepted another source node");
    response[23] = 20; response[28] = 0x11; response[29] = 3;
    require(FrameCodec::response(connection, address, 7, false, response).endCode == 0x1103, "FINS discarded error code");
    auto huge = handshakeReply; huge[4] = 0x7f;
    rejects([&] { FrameCodec::frameLength(huge); }, "FINS accepted unbounded frame");
}

void testDlt645Frames() {
    using namespace service::collector::dlt645;
    Connection connection{.version = Version::V2007, .wakeupBytes = 0};
    const std::vector<std::uint8_t> expected{0x68,1,0,0,0,0,0,0x68,0x11,4,0x33,0x33,0x33,0x33,0xb2,0x16};
    require(FrameCodec::read(connection, "000000000001", "00000000") == expected, "645-2007 read golden mismatch");
    connection.version = Version::V1997;
    const auto legacy = FrameCodec::read(connection, "000000000001", "9010");
    require(legacy[8] == 1 && legacy[9] == 2 && legacy[10] == 0x43 && legacy[11] == 0xc3, "645-1997 identifier mismatch");
    const std::array<std::uint8_t, 8> data{0,0,0,0,0x78,0x56,0x34,0x12};
    auto response = FrameCodec::frame("000000000001", 0x91, data);
    const auto parsed = FrameCodec::parse(response);
    require(parsed.address == "000000000001" && parsed.control == 0x91 && parsed.data == std::vector(data.begin(), data.end()), "645 address or 33H conversion mismatch");
    require(FrameCodec::bcdValue(std::span(data).subspan(4), 2, false) == "123456.78", "645 BCD decimal mismatch");
    const std::array<std::uint8_t, 2> negative{0x23,0x81};
    require(FrameCodec::bcdValue(negative, 2, true) == "-1.23", "645 signed BCD mismatch");
    response[10] ^= 1;
    rejects([&] { FrameCodec::parse(response); }, "645 accepted corrupt checksum");
    rejects([&] { FrameCodec::read(connection, "000000000001", "00000000"); }, "645 mixed identifier editions");
}
}

namespace {
using namespace service::collector;

const ProtocolAction& action(const std::vector<ProtocolAction>& actions, ProtocolActionKind kind) {
    const auto found = std::find_if(actions.begin(), actions.end(), [=](const auto& item) { return item.kind == kind; });
    if (found == actions.end()) throw std::runtime_error("expected protocol action missing");
    return *found;
}

bool contains(const std::vector<ProtocolAction>& actions, ProtocolActionKind kind) {
    return std::any_of(actions.begin(), actions.end(), [=](const auto& item) { return item.kind == kind; });
}

std::shared_ptr<RuntimeSnapshot> snapshot(std::string protocol) {
    auto result = std::make_shared<RuntimeSnapshot>();
    result->links.push_back({.id = "link", .mode = "TCP Client", .protocol = protocol});
    result->devices.push_back({.id = "device", .modelId = "model", .code = "000000000001",
        .linkId = "link", .targetId = "target", .protocol = protocol});
    result->devices[0].elements.push_back({.id = "point", .name = "Point", .dataType = "UINT16",
        .byteOrder = protocol == "MC" ? "LITTLE_ENDIAN" : "BIG_ENDIAN", .address = 100,
        .quantity = 1, .area = "D", .writable = true});
    return result;
}

std::vector<ProtocolAction> receive(ProtocolSession& session, std::span<const std::uint8_t> bytes) {
    return session.consume({.messageId = "packet", .linkId = "link", .connectionId = "connection",
        .receivedAtMs = 1700000000000, .bytes = bytes});
}

void testMcSession() {
    auto config = snapshot("MC");
    auto session = mc::SessionFactory{}.createSession(config->links[0], "connection", "target", config);
    auto actions = session->connected();
    require(contains(actions, ProtocolActionKind::BindDevice), "MC client did not bind");
    require(action(actions, ProtocolActionKind::Send).bytes[11] == 1, "MC did not start polling");
    const std::vector<std::uint8_t> response{0xd0,0,0,0xff,0xff,3,0,4,0,0,0,0x34,0x12};
    actions = receive(*session, std::span(response).first(4));
    require(actions.empty(), "MC published a partial frame");
    actions = receive(*session, std::span(response).subspan(4));
    require(action(actions, ProtocolActionKind::PublishParsed).parsed.valuesJson.find("4660") != std::string::npos,
        "MC session value decode failed");
    auto* commands = dynamic_cast<CommandCapabilitySession*>(session.get());
    actions = commands->execute({.id = "write", .deviceId = "device", .elements = {{"point", "12"}}});
    const auto& write = action(actions, ProtocolActionKind::Send).bytes;
    require(write[12] == 0x14 && write[21] == 12, "MC point write not compiled");
    require(!contains(actions, ProtocolActionKind::CompleteCommand), "MC completed before write ack");
    const std::vector<std::uint8_t> ack{0xd0,0,0,0xff,0xff,3,0,2,0,0,0};
    actions = receive(*session, ack);
    require(contains(actions, ProtocolActionKind::Send) && !contains(actions, ProtocolActionKind::CompleteCommand),
        "MC write did not require readback");
    auto readback = response; readback[11] = 12; readback[12] = 0;
    actions = receive(*session, readback);
    require(action(actions, ProtocolActionKind::CompleteCommand).commandId == "write", "MC readback did not complete write");
    const auto token = action(actions, ProtocolActionKind::ScheduleDeadline).deadlineToken;
    auto* deadlines = dynamic_cast<DeadlineCapabilitySession*>(session.get());
    actions = deadlines->deadline(token);
    require(contains(actions, ProtocolActionKind::Close), "MC timed-out request retained ambiguous connection");
    require(receive(*session, response).empty(), "MC closed session accepted stale frame");
}

void testFinsSession() {
    auto config = snapshot("FINS");
    auto session = fins::SessionFactory{}.createSession(config->links[0], "connection", "target", config);
    auto actions = session->connected();
    require(action(actions, ProtocolActionKind::Send).bytes.size() == 20, "FINS did not negotiate nodes first");
    const std::vector<std::uint8_t> handshake{'F','I','N','S',0,0,0,16,0,0,0,1,0,0,0,0,0,0,0,10,0,0,0,20};
    actions = receive(*session, handshake);
    const auto request = action(actions, ProtocolActionKind::Send).bytes;
    require(request[20] == 20 && request[23] == 10, "FINS session ignored negotiated nodes");
    const std::vector<std::uint8_t> response{'F','I','N','S',0,0,0,24,0,0,0,2,0,0,0,0,
        0xc0,0,2,0,10,0,0,20,0,request[25],1,1,0,0,0x12,0x34};
    actions = receive(*session, response);
    require(action(actions, ProtocolActionKind::PublishParsed).parsed.valuesJson.find("4660") != std::string::npos,
        "FINS session did not publish words");
}

void testDlt645Session() {
    auto config = snapshot("DLT645");
    auto& point = config->devices[0].elements[0];
    point.dataType = "BCD"; point.guideHex = "00000000"; point.length = 4; point.digits = 2; point.writable = false;
    auto session = dlt645::SessionFactory{}.createSession(config->links[0], "connection", "target", config);
    (void)session->connected();
    const std::vector<std::uint8_t> firstData{0,0,0,0,0x78,0x56};
    auto actions = receive(*session, dlt645::FrameCodec::frame("000000000001", 0xb1, firstData));
    require(!contains(actions, ProtocolActionKind::PublishParsed), "645 published unfinished segmented data");
    const auto& next = action(actions, ProtocolActionKind::Send).bytes;
    const auto parsed = dlt645::FrameCodec::parse(std::span(next).subspan(4));
    require(parsed.control == 0x12 && parsed.data.back() == 1, "645 subsequent request sequence wrong");
    const std::vector<std::uint8_t> lastData{0,0,0,0,0x34,0x12,1};
    actions = receive(*session, dlt645::FrameCodec::frame("000000000001", 0x92, lastData));
    const auto& published = action(actions, ProtocolActionKind::PublishParsed).parsed;
    require(published.valuesJson.find("123456.78") != std::string::npos && published.rawPayloads.size() == 2,
        "645 segmented value or packet lineage lost");
    require(dlt645::FrameCodec::bcdBytes("-1.23", 2, 2, true) == std::vector<std::uint8_t>({0x23,0x81}),
        "645 signed BCD encoding incorrect");
    rejects([] { dlt645::FrameCodec::bcdBytes("8000", 2, 0, true); }, "645 signed overflow accepted");
    rejects([] { dlt645::FrameCodec::bcdBytes("1.234", 2, 2, false); }, "645 write silently rounded precision");
    rejects([] { dlt645::FrameCodec::bcdBytes("1.", 2, 2, false); }, "645 accepted incomplete decimal");
}
}

int main() {
    try {
        testMcFrames(); testFinsFrames(); testDlt645Frames();
        testMcSession(); testFinsSession(); testDlt645Session();
        std::cout << "industrial protocol frame tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return EXIT_FAILURE;
    }
}
