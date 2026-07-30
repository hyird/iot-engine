#include "service/common/timestamp.h"
#include "service/features/gb28181/device/DeviceRegistry.h"
#include "service/features/gb28181/media/ZlmSdk.h"
#include "service/features/gb28181/sip/DigestAuth.h"
#include "service/features/gb28181/sip/SipServer.h"

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>
#include <ruvia/core/EventLoopPool.h>

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

std::string md5Hex(const std::string &input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length{};
  auto *context = EVP_MD_CTX_new();
  EVP_DigestInit_ex(context, EVP_md5(), nullptr);
  EVP_DigestUpdate(context, input.data(), input.size());
  EVP_DigestFinal_ex(context, digest.data(), &length);
  EVP_MD_CTX_free(context);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < length; ++index)
    output << std::setw(2) << static_cast<unsigned int>(digest[index]);
  return output.str();
}

std::string challengeNonce(std::string_view response) {
  constexpr std::string_view marker{"nonce=\""};
  const auto begin = response.find(marker);
  if (begin == std::string_view::npos)
    return {};
  const auto value = begin + marker.size();
  const auto end = response.find('"', value);
  return end == std::string_view::npos
             ? std::string{}
             : std::string(response.substr(value, end - value));
}

std::string sipRegister(std::uint16_t clientPort, unsigned int cseq,
                        std::string_view nonce = {}) {
  constexpr std::string_view device{"34020000001320000001"};
  constexpr std::string_view realm{"3402000000"};
  constexpr std::string_view password{"test"};
  constexpr std::string_view uri{"sip:3402000000"};
  std::string authorization;
  if (!nonce.empty()) {
    const auto ha1 = md5Hex(std::string(device) + ":" + std::string(realm) +
                            ":" + std::string(password));
    const auto ha2 = md5Hex("REGISTER:" + std::string(uri));
    const auto response = md5Hex(ha1 + ":" + std::string(nonce) +
                                 ":00000001:test-cnonce:auth:" + ha2);
    authorization = "Authorization: Digest username=\"" + std::string(device) +
                    "\", realm=\"" + std::string(realm) + "\", nonce=\"" +
                    std::string(nonce) + "\", uri=\"" + std::string(uri) +
                    "\", response=\"" + response +
                    "\", algorithm=MD5, qop=auth, nc=00000001, "
                    "cnonce=\"test-cnonce\"\r\n";
  }
  return "REGISTER " + std::string(uri) +
         " SIP/2.0\r\n"
         "Via: SIP/2.0/UDP 127.0.0.1:" +
         std::to_string(clientPort) + ";branch=z9hG4bK-register-" +
         std::to_string(cseq) +
         "\r\n"
         "From: <sip:" +
         std::string(device) + "@" + std::string(realm) +
         ">;tag=test\r\n"
         "To: <sip:" +
         std::string(device) + "@" + std::string(realm) +
         ">\r\n"
         "Contact: <sip:" +
         std::string(device) + "@127.0.0.1:" + std::to_string(clientPort) +
         ">\r\n"
         "Call-ID: gb28181-register-test\r\n"
         "CSeq: " +
         std::to_string(cseq) +
         " REGISTER\r\n"
         "Expires: 60\r\n" +
         authorization + "Content-Length: 0\r\n\r\n";
}

std::string sipKeepalive(std::uint16_t clientPort) {
  const std::string body =
      "<?xml version=\"1.0\"?>\r\n"
      "<Notify><CmdType>Keepalive</CmdType><SN>1</SN>"
      "<DeviceID>34020000001320000001</DeviceID><Status>OK</Status></Notify>";
  return "MESSAGE sip:34020000002000000001@3402000000 SIP/2.0\r\n"
         "Via: SIP/2.0/UDP 127.0.0.1:" +
         std::to_string(clientPort) +
         ";branch=z9hG4bK-keepalive\r\n"
         "From: <sip:34020000001320000001@3402000000>;tag=test\r\n"
         "To: <sip:34020000002000000001@3402000000>\r\n"
         "Call-ID: gb28181-keepalive-test\r\n"
         "CSeq: 4 MESSAGE\r\n"
         "Content-Type: Application/MANSCDP+xml\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

template <typename Receive> std::string receiveUntil(Receive receive) {
  std::string response;
  std::array<char, 4096> buffer{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
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
    std::vector<DeviceRegistry::Change> changes;
    DeviceRegistry observed(
        [&changes](const Device &, DeviceRegistry::Change change) {
          changes.push_back(change);
        });
    observed.upsertRegistration("device", "127.0.0.1:5060");
    observed.updateCatalog("device", {});
    observed.updateRecords("device", {});
    require(observed.updateMapping("device",
                                   "00000000-0000-7000-8000-000000000001"),
            "GB28181 device mapping update failed");
    observed.markOffline("device");
    require(changes ==
                std::vector<DeviceRegistry::Change>{
                    DeviceRegistry::Change::Status,
                    DeviceRegistry::Change::Catalog,
                    DeviceRegistry::Change::Records,
                    DeviceRegistry::Change::Mapping,
                    DeviceRegistry::Change::Status,
                },
            "GB28181 projection changes were not classified");

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
    sip.transport = "both";
    sip.registrationTimeoutSeconds = 1;
    sip.logging = false;

    MediaConfig media;
    media.rtpPublicIp = "127.0.0.1";
    media.playTokenSecret = "gb28181-sip-test-secret";
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
    const auto udpResponse =
        receiveUntil([&udp, &udpRemote](auto &buffer, std::error_code &error) {
          return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
        });
    require(udpResponse.starts_with("SIP/2.0 405 Method Not Allowed"),
            "Ruvia SIP worker did not answer UDP");

    const auto registerChallenge = sipRegister(udp.local_endpoint().port(), 2);
    udp.send_to(asio::buffer(registerChallenge), serverUdp);
    const auto challengeResponse =
        receiveUntil([&udp, &udpRemote](auto &buffer, std::error_code &error) {
          return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
        });
    require(challengeResponse.starts_with("SIP/2.0 401 Unauthorized"),
            "REGISTER without digest was accepted");
    const auto nonce = challengeNonce(challengeResponse);
    require(!nonce.empty(), "REGISTER challenge did not include a nonce");

    const auto authorizedRegister =
        sipRegister(udp.local_endpoint().port(), 3, nonce);
    udp.send_to(asio::buffer(authorizedRegister), serverUdp);
    const auto registerResponse =
        receiveUntil([&udp, &udpRemote](auto &buffer, std::error_code &error) {
          return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
        });
    require(registerResponse.starts_with("SIP/2.0 200 OK"),
            "valid REGISTER digest was rejected");

    udp.send_to(asio::buffer(authorizedRegister), serverUdp);
    const auto replayResponse =
        receiveUntil([&udp, &udpRemote](auto &buffer, std::error_code &error) {
          return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
        });
    require(replayResponse.starts_with("SIP/2.0 401 Unauthorized"),
            "replayed REGISTER digest was accepted");

    const auto keepalive = sipKeepalive(udp.local_endpoint().port());
    udp.send_to(asio::buffer(keepalive), serverUdp);
    const auto keepaliveResponse =
        receiveUntil([&udp, &udpRemote](auto &buffer, std::error_code &error) {
          return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
        });
    require(keepaliveResponse.starts_with("SIP/2.0 200 OK"),
            "GB28181 keepalive was not acknowledged");

    asio::ip::tcp::socket tcp(clientContext);
    tcp.connect(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), sip.port));
    const auto tcpRequest = sipOptions(tcp.local_endpoint().port(), "TCP");
    asio::write(tcp, asio::buffer(tcpRequest));
    tcp.non_blocking(true);
    const auto tcpResponse =
        receiveUntil([&tcp](auto &buffer, std::error_code &error) {
          return tcp.read_some(asio::buffer(buffer), error);
        });
    require(tcpResponse.starts_with("SIP/2.0 405 Method Not Allowed"),
            "Ruvia SIP worker did not answer TCP");

    std::this_thread::sleep_for(std::chrono::milliseconds(1250));
    server->stop();
    const auto registered = devices.findDevice("34020000001320000001");
    require(registered.has_value(), "registered device was not retained");
    require(!registered->online,
            "device did not expire after keepalive timeout");
    require(service::common::canonicalUtcTimestamp("2026-07-29 08:40:45+00") ==
                "2026-07-29T08:40:45Z",
            "offset timestamp was not normalized");
    require(service::common::canonicalUtcTimestamp(
                "2026-07-29T08:40:45", 480) == "2026-07-29T00:40:45Z",
            "GB28181 local timestamp was not normalized with device timezone");
    require(service::common::canonicalUtcTimestamp(
                "Tue Jul 28 18:24:54 CST 2026", 480)
                .empty(),
            "untyped locale timestamp was silently accepted");
    require(
        service::common::canonicalUtcTimestamp("2026-02-31T08:40:45Z").empty(),
        "invalid calendar date was silently normalized");
    pool.stop();
    pool.join();
    server.reset();
    std::cout << "gb28181 Ruvia SIP worker tests passed\n";
    return 0;
  } catch (const std::exception &error) {
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
