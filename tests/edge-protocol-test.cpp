#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "service/features/edge/protocol.h"
#include "service/features/edge/firmware.h"
#include <terminal.pb.h>

namespace {

void require(bool condition, std::string_view message) {
    if (condition)
        return;
    std::cerr << "edge protocol test failed: " << message << '\n';
    std::exit(1);
}

void testNetworkNanopbWireContract() {
    service::edge::pb::NetworkConfigRequest request;
    const std::array<std::uint8_t, 16> requestId{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    request.set_request_id(
        service::edge::protocol::bytes(requestId.data(), requestId.size()));
    auto* interface = request.add_interfaces();
    interface->set_name("eth0");
    interface->set_mode(service::edge::pb::NETWORK_ADDRESS_DHCP);
    interface->set_operation(service::edge::pb::NETWORK_CONFIG_UPSERT);
    interface->set_logical_name("lan");
    interface->set_device("eth0");
    interface->set_previous_logical_name("old");
    request.set_rollback_timeout_sec(30);

    std::string wire;
    require(request.SerializeToString(&wire), "network request serialization failed");
    const std::array<std::uint8_t, 48> nanopbWire{
        0x0a, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x12, 0x1a, 0x0a, 0x04,
        0x65, 0x74, 0x68, 0x30, 0x10, 0x01, 0x48, 0x01, 0x52, 0x03, 0x6c,
        0x61, 0x6e, 0x5a, 0x04, 0x65, 0x74, 0x68, 0x30, 0x62, 0x03, 0x6f,
        0x6c, 0x64, 0x18, 0x1e,
    };
    require(wire == std::string_view(reinterpret_cast<const char*>(nanopbWire.data()),
                                     nanopbWire.size()),
            "C++ Protobuf wire differs from the nanopb golden vector");
}

void testConfigItemNanopbWireContract() {
    service::edge::pb::ConfigItem item;
    item.set_revision(7);
    item.set_kind(service::edge::pb::CONFIG_ITEM_ENDPOINT);
    auto* endpoint = item.mutable_endpoint();
    const std::array<std::uint8_t, 16> id{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    endpoint->set_endpoint_id(service::edge::protocol::bytes(id.data(), id.size()));
    endpoint->set_name("x");
    endpoint->set_transport(service::edge::pb::TRANSPORT_ETHERNET);
    endpoint->set_mode(service::edge::pb::LINK_MODE_TCP_CLIENT);
    endpoint->set_protocol(service::edge::pb::PROTOCOL_MODBUS);
    endpoint->set_ip("1.2.3.4");
    endpoint->set_port(502);
    endpoint->set_enabled(true);
    endpoint->set_interface_name("eth0");

    std::string wire;
    require(item.SerializeToString(&wire), "config item serialization failed");
    const std::array<std::uint8_t, 53> nanopbWire{
        0x08, 0x07, 0x18, 0x01, 0x52, 0x2f, 0x0a, 0x10, 0x00, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x12, 0x01, 0x78, 0x18, 0x01, 0x20, 0x02, 0x28, 0x02,
        0x32, 0x07, 0x31, 0x2e, 0x32, 0x2e, 0x33, 0x2e, 0x34, 0x38, 0xf6,
        0x03, 0x48, 0x01, 0x52, 0x04, 0x65, 0x74, 0x68, 0x30,
    };
    require(wire == std::string_view(reinterpret_cast<const char*>(nanopbWire.data()),
                                     nanopbWire.size()),
            "config item wire differs from the nanopb digest contract");
}

void testEnvelopeRoundTrip() {
    auto envelope = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000002", 42, 7);
    envelope.mutable_ping()->set_nonce(1234);
    const auto wire = service::edge::protocol::encode(envelope);
    require(!wire.empty(), "envelope serialization failed");

    service::edge::pb::Envelope decoded;
    require(service::edge::protocol::decode(wire, decoded), "envelope parse failed");
    require(decoded.protocol_version() == service::edge::protocol::kProtocolVersion,
            "protocol version changed");
    require(decoded.session_epoch() == 42 && decoded.sequence() == 7,
            "session identity changed");
    require(decoded.payload_case() == service::edge::pb::Envelope::kPing &&
                decoded.ping().nonce() == 1234,
            "oneof payload changed");

    std::string oversized(service::edge::protocol::kMaxMessageSize + 1, '\0');
    require(!service::edge::protocol::decode(oversized, decoded),
            "oversized envelope was accepted");
}

void testCompatibleProtocolVersionContract() {
    require(!service::edge::protocol::isCurrentProtocolVersion(2),
            "legacy protocol was reported as current");
    require(service::edge::protocol::supportsProtocolVersion(2) &&
                service::edge::protocol::supportsProtocolVersion(3) &&
                service::edge::protocol::supportsProtocolVersion(4) &&
                service::edge::protocol::supportsProtocolVersion(5),
            "supported edgenode protocol range changed");
    require(!service::edge::protocol::supportsProtocolVersion(1) &&
                !service::edge::protocol::supportsProtocolVersion(6),
            "unsupported protocol version was accepted");
    require(service::edge::protocol::isCurrentProtocolVersion(
                service::edge::protocol::kProtocolVersion),
            "current protocol version was rejected");
    require(service::edge::protocol::kProtocolVersion == 5,
            "terminal flow control did not advance the wire protocol");
    require(!service::edge::protocol::isCurrentProtocolVersion(6),
            "future protocol version was reported as current");

    auto envelope = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000002");
    envelope.mutable_heartbeat_ack()->set_platform_time_ms(1234);
    const std::string platform(16, '\x01');
    const std::string node(16, '\x02');
    envelope.set_protocol_version(2);
    service::edge::protocol::bindSession(envelope, platform, node, 42, 7);
    const auto wire = service::edge::protocol::encode(envelope);
    require(!wire.empty(), "strict protocol envelope serialization failed");

    service::edge::pb::Envelope decoded;
    require(service::edge::protocol::decode(wire, decoded),
            "strict protocol envelope parse failed");
    require(decoded.protocol_version() == service::edge::protocol::kProtocolVersion,
            "session binding preserved a stale protocol version");
    require(decoded.platform_id() == platform && decoded.node_id() == node &&
                decoded.session_epoch() == 42 && decoded.sequence() == 7,
            "strict protocol session binding changed identity");
    require(decoded.payload_case() == service::edge::pb::Envelope::kHeartbeatAck &&
                decoded.heartbeat_ack().platform_time_ms() == 1234,
            "strict protocol session binding changed payload");

    service::edge::protocol::bindSession(envelope, platform, node, 43, 8, 3);
    require(envelope.protocol_version() == 3 && envelope.session_epoch() == 43 &&
                envelope.sequence() == 8,
            "legacy session binding did not preserve the negotiated protocol");
}

void testTerminalOpenedContract() {
    auto envelope = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000002", 42, 7);
    const std::string terminalId(16, '\x03');
    envelope.mutable_terminal_opened()->set_terminal_id(terminalId);
    const auto wire = service::edge::protocol::encode(envelope);
    require(!wire.empty(), "terminal opened acknowledgement was not encoded");

    service::edge::pb::Envelope decoded;
    require(service::edge::protocol::decode(wire, decoded),
            "terminal opened acknowledgement was not decoded");
    require(decoded.payload_case() == service::edge::pb::Envelope::kTerminalOpened &&
                decoded.terminal_opened().terminal_id() == terminalId,
            "terminal opened acknowledgement changed identity");
}

void testTerminalFlowControlContract() {
    auto envelope = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000002", 42, 8);
    const std::string terminalId(16, '\x04');
    auto* data = envelope.mutable_terminal_data();
    data->set_terminal_id(terminalId);
    data->set_data("input");
    data->set_sequence(9);
    const auto wire = service::edge::protocol::encode(envelope);
    require(!wire.empty(), "sequenced terminal data was not encoded");
    service::edge::pb::Envelope decoded;
    require(service::edge::protocol::decode(wire, decoded) &&
                decoded.terminal_data().sequence() == 9,
            "terminal data sequence did not round trip");

    auto ack = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000002", 42, 9);
    ack.mutable_terminal_data_ack()->set_terminal_id(terminalId);
    ack.mutable_terminal_data_ack()->set_sequence(9);
    const auto ackWire = service::edge::protocol::encode(ack);
    require(service::edge::protocol::decode(ackWire, decoded) &&
                decoded.payload_case() ==
                    service::edge::pb::Envelope::kTerminalDataAck &&
                decoded.terminal_data_ack().sequence() == 9,
            "terminal data acknowledgement did not round trip");
}

void testLegacyEdgenodeContract() {
    constexpr std::uint32_t legacyVersion = 3;
    const std::string nodeId = "00000000-0000-7000-8000-000000000002";
    const std::string platform(16, '\x01');
    const std::string node(16, '\x02');
    const std::string terminalId(16, '\x05');

    auto helloAck = service::edge::protocol::outbound(nodeId, 77, 1, legacyVersion);
    auto* negotiated = helloAck.mutable_hello_ack();
    negotiated->set_assigned_node_id(node);
    negotiated->set_session_epoch(77);
    negotiated->set_negotiated_protocol_version(legacyVersion);
    service::edge::protocol::bindSession(helloAck, platform, node, 77, 1,
                                         legacyVersion);
    auto wire = service::edge::protocol::encode(helloAck);
    service::edge::pb::Envelope decoded;
    require(service::edge::protocol::decode(wire, decoded) &&
                decoded.protocol_version() == legacyVersion &&
                decoded.hello_ack().negotiated_protocol_version() == legacyVersion,
            "legacy hello negotiation did not round trip");

    auto open = service::edge::protocol::outbound(nodeId, 77, 2, legacyVersion);
    auto* terminalOpen = open.mutable_terminal_open();
    terminalOpen->set_terminal_id(terminalId);
    terminalOpen->set_ticket("legacy-browser-ticket");
    terminalOpen->set_columns(120);
    terminalOpen->set_rows(30);
    wire = service::edge::protocol::encode(open);
    require(service::edge::protocol::decode(wire, decoded) &&
                decoded.protocol_version() == legacyVersion &&
                decoded.terminal_open().ticket().size() >= 16,
            "legacy terminal ticket contract was not preserved");

    auto data = service::edge::protocol::outbound(nodeId, 77, 3, legacyVersion);
    data.mutable_terminal_data()->set_terminal_id(terminalId);
    data.mutable_terminal_data()->set_data("legacy input");
    wire = service::edge::protocol::encode(data);
    require(service::edge::protocol::decode(wire, decoded) &&
                decoded.terminal_data().sequence() == 0,
            "legacy unsequenced terminal data contract changed");
}

void testPublicBaseUrlConfiguration() {
    require(!service::edge::protocol::configurePublicBaseUrl("ftp://secondary.example"),
            "invalid public platform URL was accepted");
    require(service::edge::protocol::configurePublicBaseUrl("https://secondary.example/"),
            "valid public platform URL was rejected");
    require(service::edge::protocol::publicBaseUrl() == "https://secondary.example",
            "public platform URL was not normalized");
    std::uint8_t expected[16]{};
    require(service::edge::protocol::uuidBytes(service::edge::protocol::platformId(), expected),
            "internal platform id is invalid");
    const auto envelope = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000003");
    require(envelope.platform_id() == service::edge::protocol::bytes(expected, 16),
            "outbound envelope ignored the internal platform id");
    require(service::edge::protocol::configurePublicBaseUrl(
                service::edge::protocol::kDefaultPublicBaseUrl),
            "default public platform URL could not be restored");
}

void testSessionPlatformIdentityIsInternal() {
    require(!service::edge::protocol::validSessionPlatformId({}),
            "empty session platform id was accepted");
    require(!service::edge::protocol::validSessionPlatformId(std::string(16, '\0')),
            "zero session platform id was accepted");
    require(service::edge::protocol::validSessionPlatformId(std::string(16, '\1')),
            "generated session platform id was rejected");
}

void testModemProfileRoundTrip() {
    auto envelope = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000002");
    auto* request = envelope.mutable_modem_control_request();
    request->set_request_id(std::string(16, '\1'));
    request->set_action(service::edge::pb::MODEM_CONTROL_APPLY_PROFILE);
    request->set_apn("private.mnc001.mcc460.gprs");
    request->set_automatic_apn(false);
    request->set_username("edge-user");
    request->set_password("secret");
    request->set_pdp_type(service::edge::pb::MODEM_PDP_IPV4V6);
    request->set_auth_type(service::edge::pb::MODEM_AUTH_PAP_OR_CHAP);
    request->set_pin_code("1234");
    request->set_redial_after_apply(true);

    const auto wire = service::edge::protocol::encode(envelope);
    require(!wire.empty(), "modem profile serialization failed");
    service::edge::pb::Envelope decoded;
    require(service::edge::protocol::decode(wire, decoded),
            "modem profile parse failed");
    const auto& profile = decoded.modem_control_request();
    require(profile.action() == service::edge::pb::MODEM_CONTROL_APPLY_PROFILE &&
                profile.apn() == "private.mnc001.mcc460.gprs" &&
                !profile.automatic_apn() && profile.username() == "edge-user" &&
                profile.password() == "secret" &&
                profile.pdp_type() == service::edge::pb::MODEM_PDP_IPV4V6 &&
                profile.auth_type() == service::edge::pb::MODEM_AUTH_PAP_OR_CHAP &&
                profile.pin_code() == "1234" && profile.redial_after_apply(),
            "modem profile fields did not round-trip");

    request->set_apn(std::string(101, 'a'));
    require(service::edge::protocol::encode(envelope).empty(),
            "oversized APN escaped the nanopb wire limit");
}

void testNanopbBounds() {
    auto envelope = service::edge::protocol::outbound(
        "00000000-0000-7000-8000-000000000002");
    auto* request = envelope.mutable_network_config_request();
    request->set_request_id(std::string(16, '\1'));
    request->add_interfaces()->set_logical_name(std::string(33, 'x'));
    require(service::edge::protocol::encode(envelope).empty(),
            "nanopb max_length annotation was ignored");

    std::string uncheckedWire;
    require(envelope.SerializeToString(&uncheckedWire),
            "unchecked envelope serialization failed");
    service::edge::pb::Envelope decoded;
    require(!service::edge::protocol::decode(uncheckedWire, decoded),
            "nanopb-incompatible envelope was accepted");
    require(decoded.payload_case() == service::edge::pb::Envelope::PAYLOAD_NOT_SET,
            "failed decode retained an invalid payload");
}

void testWebTerminalProtobuf() {
    ::iot::edge::terminal::v1::WebTerminalFrame frame;
    frame.mutable_resize()->set_columns(120);
    frame.mutable_resize()->set_rows(30);

    std::string wire;
    require(frame.SerializeToString(&wire), "web terminal frame serialization failed");
    const std::array<std::uint8_t, 6> expected{
        0x1a, 0x04, 0x08, 0x78, 0x10, 0x1e,
    };
    require(wire == std::string_view(reinterpret_cast<const char*>(expected.data()),
                                     expected.size()),
            "web terminal frame differs from the browser golden vector");

    ::iot::edge::terminal::v1::WebTerminalFrame decoded;
    require(decoded.ParseFromString(wire), "web terminal frame parse failed");
    require(decoded.payload_case() ==
                ::iot::edge::terminal::v1::WebTerminalFrame::kResize &&
                decoded.resize().columns() == 120 && decoded.resize().rows() == 30,
            "web terminal resize payload changed");
}

void testFirmwareRequestDefersVersionToNodeHello() {
    service::edge::pb::FirmwareUpdateRequest request;
    require(service::edge::firmware::populateUpdateRequest(
                request, std::string(16, '\1'), "https://example.test/firmware.bin",
                std::string(32, '\2'), 1024, true),
            "valid firmware request metadata was rejected");
    require(request.version().empty(),
            "firmware request must not infer or require a target version");
    require(request.size_bytes() == 1024 && request.keep_settings(),
            "firmware request metadata changed unexpectedly");
    service::edge::pb::FirmwareUpdateRequest empty;
    require(!service::edge::firmware::populateUpdateRequest(
                empty, std::string(16, '\1'), "https://example.test/firmware.bin",
                std::string(32, '\2'), 0, true),
            "zero-length firmware request was accepted");
}

void testCommandResultRequiresTerminalState() {
    require(!service::edge::protocol::terminalCommandResultState(
                service::edge::pb::COMMAND_STATE_UNSPECIFIED) &&
                !service::edge::protocol::terminalCommandResultState(
                    service::edge::pb::COMMAND_STATE_ACCEPTED) &&
                !service::edge::protocol::terminalCommandResultState(
                    service::edge::pb::COMMAND_STATE_RUNNING),
            "non-terminal command progress completed the command");
    require(service::edge::protocol::terminalCommandResultState(
                service::edge::pb::COMMAND_STATE_SUCCEEDED) &&
                service::edge::protocol::terminalCommandResultState(
                    service::edge::pb::COMMAND_STATE_TIMED_OUT) &&
                service::edge::protocol::terminalCommandResultState(
                    service::edge::pb::COMMAND_STATE_FAILED),
            "terminal command result state was rejected");
}

} // namespace

int main() {
    require(service::edge::protocol::validImei("490154203237518"), "valid IMEI rejected");
    require(!service::edge::protocol::validImei("490154203237519"), "bad IMEI accepted");
    testNetworkNanopbWireContract();
    testConfigItemNanopbWireContract();
    testEnvelopeRoundTrip();
    testCompatibleProtocolVersionContract();
    testTerminalOpenedContract();
    testTerminalFlowControlContract();
    testLegacyEdgenodeContract();
    testPublicBaseUrlConfiguration();
    testSessionPlatformIdentityIsInternal();
    testModemProfileRoundTrip();
    testNanopbBounds();
    testWebTerminalProtobuf();
    testFirmwareRequestDefersVersionToNodeHello();
    testCommandResultRequiresTerminalState();
    std::cout << "edge protocol tests passed\n";
}
