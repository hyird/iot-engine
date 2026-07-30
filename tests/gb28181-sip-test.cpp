#include "service/features/gb28181/device/DeviceRegistry.h"
#include "service/features/gb28181/media/ZlmSdk.h"
#include "service/features/gb28181/sip/SipServer.h"

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/write.hpp>
#include <ruvia/core/EventLoopPool.h>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::uint16_t reserveLocalPort() {
    asio::io_context context;
    asio::ip::tcp::acceptor reservation(
        context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    return port;
}

std::string sipOptions(std::uint16_t clientPort, std::string_view transport) {
    return "OPTIONS sip:34020000002000000001@127.0.0.1 SIP/2.0\r\n"
           "Via: SIP/2.0/" +
           std::string(transport) + " 127.0.0.1:" + std::to_string(clientPort) +
           ";branch=z9hG4bK-test\r\n"
           "From: <sip:34020000001320000001@3402000000>;tag=test\r\n"
           "To: <sip:34020000002000000001@3402000000>\r\n"
           "Call-ID: gb28181-sip-worker-test\r\n"
           "CSeq: 1 OPTIONS\r\n"
           "Content-Length: 0\r\n\r\n";
}

template <typename Receive>
std::string receiveUntil(Receive receive) {
    std::string response;
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        const auto size = receive(buffer, error);
        if (!error) {
            response.append(buffer.data(), size);
            if (response.find("\r\n\r\n") != std::string::npos)
                return response;
        } else if (error != asio::error::would_block &&
                   error != asio::error::try_again) {
            throw std::runtime_error("SIP client receive failed: " + error.message());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return response;
}

} // namespace

int main() {
    ruvia::EventLoopPool pool(
        ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 128});
    std::unique_ptr<SipServer> server;
    try {
        pool.start();
        auto loop = pool.loop(0);
        require(loop.valid(), "Ruvia SIP worker did not start");

        SipConfig sip;
        sip.domain = "3402000000";
        sip.id = "34020000002000000001";
        sip.host = "127.0.0.1";
        sip.publicIp = "127.0.0.1";
        sip.port = reserveLocalPort();
        sip.password = "test";
        sip.logging = false;

        MediaConfig media;
        media.rtpPublicIp = "127.0.0.1";
        DeviceRegistry devices;
        ZlmSdk zlm(media);
        server = std::make_unique<SipServer>(sip, media, devices, zlm, loop);
        server->start();

        asio::io_context clientContext;
        const asio::ip::udp::endpoint serverUdp(asio::ip::address_v4::loopback(),
                                                 sip.port);
        asio::ip::udp::socket udp(clientContext,
                                  asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
        const auto udpRequest = sipOptions(udp.local_endpoint().port(), "UDP");
        udp.send_to(asio::buffer(udpRequest), serverUdp);
        udp.non_blocking(true);
        asio::ip::udp::endpoint udpRemote;
        const auto udpResponse = receiveUntil(
            [&udp, &udpRemote](auto& buffer, std::error_code& error) {
                return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            });
        require(udpResponse.starts_with("SIP/2.0 405 Method Not Allowed"),
                "Ruvia SIP worker did not answer UDP");

        asio::ip::tcp::socket tcp(clientContext);
        tcp.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(),
                                            sip.port));
        const auto tcpRequest = sipOptions(tcp.local_endpoint().port(), "TCP");
        asio::write(tcp, asio::buffer(tcpRequest));
        tcp.non_blocking(true);
        const auto tcpResponse =
            receiveUntil([&tcp](auto& buffer, std::error_code& error) {
                return tcp.read_some(asio::buffer(buffer), error);
            });
        require(tcpResponse.starts_with("SIP/2.0 405 Method Not Allowed"),
                "Ruvia SIP worker did not answer TCP");

        server->stop();
        pool.stop();
        pool.join();
        server.reset();
        std::cout << "gb28181 Ruvia SIP worker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        try {
            if (server)
                server->stop();
            pool.stop();
            pool.join();
        } catch (...) {
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
