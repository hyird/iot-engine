#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "service/modules/edge/edge.protocol.h"
#include <web_terminal.pb.h>

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
    request.set_rollback_timeout_sec(30);

    std::string wire;
    require(request.SerializeToString(&wire), "network request serialization failed");
    const std::array<std::uint8_t, 43> nanopbWire{
        0x0a, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x12, 0x15, 0x0a, 0x04,
        0x65, 0x74, 0x68, 0x30, 0x10, 0x01, 0x48, 0x01, 0x52, 0x03, 0x6c,
        0x61, 0x6e, 0x5a, 0x04, 0x65, 0x74, 0x68, 0x30, 0x18, 0x1e,
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

void testCompressedTerminalWireContract() {
    service::edge::pb::TerminalData data;
    data.set_terminal_id(std::string(16, '\1'));
    data.set_data("abc");
    data.set_compressed(true);

    std::string wire;
    require(data.SerializeToString(&wire), "compressed terminal serialization failed");
    const std::array<std::uint8_t, 25> nanopbWire{
        0x0a, 0x10, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x12, 0x03, 0x61, 0x62, 0x63, 0x18, 0x01,
    };
    require(wire == std::string_view(reinterpret_cast<const char*>(nanopbWire.data()),
                                     nanopbWire.size()),
            "compressed terminal wire differs from the nanopb contract");
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

} // namespace

int main() {
    require(service::edge::protocol::validImei("490154203237518"), "valid IMEI rejected");
    require(!service::edge::protocol::validImei("490154203237519"), "bad IMEI accepted");
    testNetworkNanopbWireContract();
    testConfigItemNanopbWireContract();
    testEnvelopeRoundTrip();
    testNanopbBounds();
    testCompressedTerminalWireContract();
    testWebTerminalProtobuf();
    std::cout << "edge protocol tests passed\n";
}
