#include "service/common/timestamp.h"
#include "service/features/gb28181/device/DeviceRegistry.h"
#include "service/features/gb28181/media/ZlmSdk.h"
#include "service/features/gb28181/sip/DigestAuth.h"
#include "service/features/gb28181/sip/SipFloodGuard.h"
#include "service/features/gb28181/sip/SipMessage.h"
#include "service/features/gb28181/sip/SipServer.h"

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>
#include <ruvia/core/EventLoopPool.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <optional>
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
  // The SIP test uses the same port for both transports. Reserving only TCP can
  // select a Windows-excluded UDP port and makes repeated runs intermittently fail.
  for (int attempt = 0; attempt < 32; ++attempt) {
    std::error_code error;
    asio::ip::tcp::acceptor tcpReservation(context);
    tcpReservation.open(asio::ip::tcp::v4(), error);
    if (error)
      continue;
    tcpReservation.bind(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), error);
    if (error)
      continue;
    const auto port = tcpReservation.local_endpoint(error).port();
    if (error)
      continue;
    asio::ip::udp::socket udpReservation(context);
    udpReservation.open(asio::ip::udp::v4(), error);
    if (error)
      continue;
    udpReservation.bind(
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port), error);
    if (error)
      continue;
    udpReservation.close();
    tcpReservation.close();
    return port;
  }
  throw std::runtime_error("could not reserve a shared TCP/UDP loopback port");
}

std::uint16_t reserveLocalUdpPort(const asio::ip::address &address) {
  asio::io_context context;
  asio::ip::udp::socket reservation(context);
  std::error_code error;
  reservation.open(address.is_v6() ? asio::ip::udp::v6()
                                   : asio::ip::udp::v4(),
                   error);
  if (error)
    throw std::runtime_error("could not open UDP loopback reservation socket: " +
                             error.message());
  reservation.bind(asio::ip::udp::endpoint(address, 0), error);
  if (error)
    throw std::runtime_error("could not reserve UDP loopback port: " +
                             error.message());
  const auto port = reservation.local_endpoint(error).port();
  if (error)
    throw std::runtime_error("could not read UDP loopback reservation port: " +
                             error.message());
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

std::string xmlValue(std::string_view body, std::string_view name) {
  const auto open = "<" + std::string(name) + ">";
  const auto close = "</" + std::string(name) + ">";
  const auto begin = body.find(open);
  if (begin == std::string_view::npos)
    return {};
  const auto value = begin + open.size();
  const auto end = body.find(close, value);
  return end == std::string_view::npos
             ? std::string{}
             : std::string(body.substr(value, end - value));
}

std::string sipRegister(std::uint16_t clientPort, unsigned int cseq,
                        std::string_view nonce = {},
                        std::string_view fromDevice = "34020000001320000001",
                        std::string_view authUsername = {},
                        std::string_view nonceCount = "00000001",
                        std::string_view cnonce = "test-cnonce") {
  constexpr std::string_view realm{"3402000000"};
  constexpr std::string_view password{"test"};
  constexpr std::string_view uri{"sip:3402000000"};
  const auto digestUser =
      authUsername.empty() ? std::string(fromDevice) : std::string(authUsername);
  std::string authorization;
  if (!nonce.empty()) {
    const auto ha1 = md5Hex(digestUser + ":" + std::string(realm) +
                            ":" + std::string(password));
    const auto ha2 = md5Hex("REGISTER:" + std::string(uri));
    const auto response = md5Hex(ha1 + ":" + std::string(nonce) +
                                 ":" + std::string(nonceCount) + ":" +
                                 std::string(cnonce) + ":auth:" + ha2);
    authorization = "Authorization: Digest username=\"" + digestUser +
                    "\", realm=\"" + std::string(realm) + "\", nonce=\"" +
                    std::string(nonce) + "\", uri=\"" + std::string(uri) +
                    "\", response=\"" + response +
                    "\", algorithm=MD5, qop=auth, nc=" +
                    std::string(nonceCount) + ", cnonce=\"" +
                    std::string(cnonce) + "\"\r\n";
  }
  return "REGISTER " + std::string(uri) +
         " SIP/2.0\r\n"
         "Via: SIP/2.0/UDP 127.0.0.1:" +
         std::to_string(clientPort) + ";branch=z9hG4bK-register-" +
         std::to_string(cseq) +
          "\r\n"
          "From: <sip:" +
          std::string(fromDevice) + "@" + std::string(realm) +
          ">;tag=test\r\n"
          "To: <sip:" +
          std::string(fromDevice) + "@" + std::string(realm) +
          ">\r\n"
          "Contact: <sip:" +
          std::string(fromDevice) + "@127.0.0.1:" + std::to_string(clientPort) +
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

std::string catalogMessage(std::string_view ptzType) {
  const auto body =
      "<?xml version=\"1.0\"?>\r\n"
      "<Response><CmdType>Catalog</CmdType><SN>5</SN>"
      "<DeviceID>34020000001320000001</DeviceID>"
      "<DeviceList Num=\"1\"><Item>"
      "<DeviceID>34020000001320000001</DeviceID>"
      "<Name>Main channel</Name>"
      "<Manufacturer>test</Manufacturer>"
      "<Status>ON</Status>"
      "<Info><PTZType>" +
      std::string(ptzType) +
      "</PTZType></Info>"
      "</Item></DeviceList></Response>";
  return "MESSAGE sip:34020000002000000001@3402000000 SIP/2.0\r\n"
         "Via: SIP/2.0/UDP 127.0.0.1:0;branch=z9hG4bK-catalog\r\n"
         "From: <sip:34020000001320000001@3402000000>;tag=catalog\r\n"
         "To: <sip:34020000002000000001@3402000000>\r\n"
         "Call-ID: gb28181-catalog-test\r\n"
         "CSeq: 10 MESSAGE\r\n"
         "Content-Type: Application/MANSCDP+xml\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

template <typename Receive>
std::string receiveUntil(Receive receive,
                         std::chrono::milliseconds timeout =
                             std::chrono::seconds(3)) {
  std::string response;
  std::array<char, 4096> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
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

template <typename Receive, typename Predicate>
std::optional<SipMessage>
receiveSipMatching(Receive receive, Predicate predicate,
                   std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    auto raw = receiveUntil(receive, std::max(std::chrono::milliseconds(1),
                                             remaining));
    if (raw.empty())
      return std::nullopt;
    auto message = SipMessage::parse(raw);
    if (message.has_value() && predicate(*message))
      return message;
  }
  return std::nullopt;
}

std::string exchangeUdp(asio::ip::udp::socket &client,
                        const asio::ip::udp::endpoint &server,
                        std::string_view request) {
  client.send_to(asio::buffer(request), server);
  asio::ip::udp::endpoint remote;
  return receiveUntil(
      [&client, &remote](auto &buffer, std::error_code &error) {
        return client.receive_from(asio::buffer(buffer), remote, 0, error);
      });
}

void verifyKeepaliveExpiry(std::shared_ptr<SipServer> &server, SipConfig sip,
                           const MediaConfig &media, DeviceRegistry &devices,
                           ZlmSdk &zlm, const ruvia::EventLoop &loop,
                           asio::io_context &clientContext) {
  server->stop();
  server.reset();

  sip.port = reserveLocalPort();
  sip.transport = "udp";
  sip.registrationTimeoutSeconds = 1;
  server = std::make_shared<SipServer>(sip, media, devices, zlm, loop);
  server->start();

  const asio::ip::udp::endpoint endpoint(asio::ip::address_v4::loopback(),
                                         sip.port);
  asio::ip::udp::socket client(
      clientContext, asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
  client.non_blocking(true);

  const auto challenge = exchangeUdp(
      client, endpoint, sipRegister(client.local_endpoint().port(), 20));
  const auto nonce = challengeNonce(challenge);
  require(challenge.starts_with("SIP/2.0 401 Unauthorized") && !nonce.empty(),
          "expiry test REGISTER challenge was not received");

  const auto registration = exchangeUdp(
      client, endpoint,
      sipRegister(client.local_endpoint().port(), 21, nonce));
  require(registration.starts_with("SIP/2.0 200 OK"),
          "expiry test REGISTER was rejected");

  const auto keepalive = exchangeUdp(
      client, endpoint, sipKeepalive(client.local_endpoint().port()));
  require(keepalive.starts_with("SIP/2.0 200 OK"),
          "expiry test keepalive was not acknowledged");

  std::this_thread::sleep_for(std::chrono::milliseconds(1250));
  server->stop();
  const auto registered = devices.findDevice("34020000001320000001");
  require(registered.has_value(), "registered device was not retained");
  require(!registered->online, "device did not expire after keepalive timeout");
}

bool socketClosesWithin(asio::ip::tcp::socket &socket,
                        std::chrono::milliseconds timeout) {
  socket.non_blocking(true);
  std::array<char, 256> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code error;
    (void)socket.read_some(asio::buffer(buffer), error);
    if (error == asio::error::eof ||
        error == asio::error::connection_reset ||
        error == asio::error::connection_aborted ||
        error == asio::error::operation_aborted) {
      return true;
    }
    if (error != asio::error::would_block && error != asio::error::try_again) {
      throw std::runtime_error("SIP client close probe failed: " +
                               error.message());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

std::string inviteOkWithCSeqMethod(const SipMessage &invite,
                                   std::string_view cseqMethod) {
  const auto inviteCSeq = invite.header("CSeq");
  const auto space = inviteCSeq.find(' ');
  const auto cseqNumber =
      space == std::string::npos ? inviteCSeq : inviteCSeq.substr(0, space);
  return "SIP/2.0 200 OK\r\n"
         "Via: " +
         invite.header("Via") +
         "\r\n"
         "From: " +
         invite.header("From") +
         "\r\n"
         "To: " +
         invite.header("To") +
         ";tag=device-ok\r\n"
         "Call-ID: " +
         invite.header("Call-ID") +
         "\r\n"
         "CSeq: " +
         cseqNumber + " " + std::string(cseqMethod) +
         "\r\n"
         "Content-Length: 0\r\n\r\n";
}

std::string recordInfoMessage(std::string_view sn, std::string_view deviceId) {
  const auto body =
      "<?xml version=\"1.0\"?>\r\n"
      "<Response><CmdType>RecordInfo</CmdType><SN>" +
      std::string(sn) + "</SN><DeviceID>" + std::string(deviceId) +
      "</DeviceID><SumNum>1</SumNum><RecordList Num=\"1\"><Item>"
      "<DeviceID>34020000001320000001</DeviceID>"
      "<Name>bad-sn-record</Name>"
      "<FilePath>/record/bad-sn.mp4</FilePath>"
      "<Address>record</Address>"
      "<StartTime>2026-07-29T08:40:45Z</StartTime>"
      "<EndTime>2026-07-29T08:41:45Z</EndTime>"
      "<Type>time</Type>"
      "</Item></RecordList></Response>";
  return "MESSAGE sip:34020000002000000001@3402000000 SIP/2.0\r\n"
         "Via: SIP/2.0/UDP 127.0.0.1:0;branch=z9hG4bK-record-info\r\n"
         "From: <sip:" +
         std::string(deviceId) +
         "@3402000000>;tag=record-info\r\n"
         "To: <sip:34020000002000000001@3402000000>\r\n"
         "Call-ID: gb28181-record-info-test\r\n"
         "CSeq: 20 MESSAGE\r\n"
         "Content-Type: Application/MANSCDP+xml\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

} // namespace

int main() {
  ruvia::EventLoopPool pool(
      ruvia::EventLoopPoolOptions{.loopCount = 1, .mailboxCapacity = 128});
  std::shared_ptr<SipServer> server;
  std::unique_ptr<DeviceRegistry> devices;
  std::unique_ptr<ZlmSdk> zlm;
  std::vector<std::pair<std::string, unsigned int>> viewerCounts;
  std::mutex viewerCountsMutex;
  std::condition_variable viewerCountsChanged;
  try {
    {
      SipUnsupportedRequestGuard guard(2.0, 1.0, std::chrono::seconds(2));
      const auto started = SipUnsupportedRequestGuard::Clock::time_point{};
      require(!SipUnsupportedRequestGuard::requiresGuard(
                  "REGISTER sip:test SIP/2.0\r\n\r\n"),
              "SIP flood guard throttled REGISTER");
      require(!SipUnsupportedRequestGuard::requiresGuard(
                  "MESSAGE sip:test SIP/2.0\r\n\r\n"),
              "SIP flood guard throttled MESSAGE");
      require(!SipUnsupportedRequestGuard::requiresGuard(
                  "SIP/2.0 200 OK\r\n\r\n"),
              "SIP flood guard throttled a response");
      require(guard.inspect("OPTIONS sip:test SIP/2.0\r\n\r\n", started).allowed,
              "SIP flood guard rejected the first unsupported request");
      require(guard.inspect("INVITE sip:test SIP/2.0\r\n\r\n", started).allowed,
              "SIP flood guard rejected the configured burst");
      require(!guard.inspect("OPTIONS sip:test SIP/2.0\r\n\r\n", started).allowed,
              "SIP flood guard exceeded the configured burst");
      require(guard.inspect("OPTIONS sip:test SIP/2.0\r\n\r\n",
                            started + std::chrono::seconds(1)).allowed,
              "SIP flood guard did not refill its budget");
      const auto suppressed = guard.inspect(
          "INVITE sip:test SIP/2.0\r\n\r\n",
          started + std::chrono::seconds(2));
      require(suppressed.allowed,
              "SIP flood guard did not preserve its sustained request budget");
      const auto report = guard.inspect(
          "INVITE sip:test SIP/2.0\r\n\r\n",
          started + std::chrono::seconds(2));
      require(!report.allowed && report.suppressedToReport == 2,
              "SIP flood guard did not aggregate suppressed requests");
    }

    require(!SipMessage::parse(
                 "MESSAGE sip:platform@example.test SIP/2.0\r\n"
                 "Content-Length: 0\r\n\r\n<Notify>smuggled</Notify>")
                 .has_value(),
            "SIP parser accepted bytes beyond the declared Content-Length");
    require(!SipMessage::parse(
                 "MESSAGE sip:platform@example.test SIP/2.0\r\n"
                 "Content-Length: 8\r\n\r\nshort")
                 .has_value(),
            "SIP parser accepted a truncated declared body");
    require(!SipMessage::parse("SIP/2.0 200x OK\r\nContent-Length: 0\r\n\r\n")
                 .has_value(),
            "SIP parser accepted a non-decimal response status");
    require(!SipMessage::parse(
                 "REGISTER sip:platform@example.test HTTP/1.1\r\nContent-Length: 0\r\n\r\n")
                 .has_value(),
            "SIP parser accepted a request with a non-SIP protocol version");
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
    sip.registrationTimeoutSeconds = 30;
    sip.viewerLeaseTimeoutSeconds = 1;
    sip.logging = false;

    MediaConfig media;
    media.rtpPublicIp = "127.0.0.1";
    media.playTokenSecret = "gb28181-sip-test-secret";
    media.logLevel = 4;
    media.httpPort = 0;
    media.rtspPort = 0;
    media.rtmpPort = 0;
    media.rtcPort = 0;
    media.srtPort = 0;
    media.rtpPortRangeStart = 0;
    media.rtpPortRangeEnd = 0;
    devices = std::make_unique<DeviceRegistry>();
    zlm = std::make_unique<ZlmSdk>(media);
    zlm->start();
    server = std::make_shared<SipServer>(
        sip, media, *devices, *zlm, loop,
        [&viewerCounts, &viewerCountsMutex,
         &viewerCountsChanged](const std::string &stream,
                               unsigned int count) {
          std::lock_guard lock(viewerCountsMutex);
          viewerCounts.emplace_back(stream, count);
          viewerCountsChanged.notify_all();
        });
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

    constexpr std::string_view spoofedDevice{"34020000001320009999"};
    const auto spoofedRegister =
        sipRegister(udp.local_endpoint().port(), 4, nonce, spoofedDevice,
                    "34020000001320000001", "00000002", "spoof-cnonce");
    udp.send_to(asio::buffer(spoofedRegister), serverUdp);
    const auto spoofedRegisterResponse =
        receiveUntil([&udp, &udpRemote](auto &buffer, std::error_code &error) {
          return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
        });
    require(spoofedRegisterResponse.starts_with("SIP/2.0 403 Forbidden"),
            "REGISTER accepted a Digest username that did not match From");
    require(!devices->findDevice(std::string(spoofedDevice)).has_value(),
            "REGISTER created a device from an unauthenticated From identity");

    asio::ip::udp::socket spoofUdp(clientContext,
                                   asio::ip::udp::endpoint(asio::ip::udp::v4(),
                                                          0));
    spoofUdp.non_blocking(true);
    asio::ip::udp::endpoint spoofRemote;
    const auto spoofKeepalive = sipKeepalive(spoofUdp.local_endpoint().port());
    spoofUdp.send_to(asio::buffer(spoofKeepalive), serverUdp);
    const auto spoofKeepaliveAck =
        receiveSipMatching(
            [&spoofUdp, &spoofRemote](auto &buffer, std::error_code &error) {
              return spoofUdp.receive_from(asio::buffer(buffer), spoofRemote, 0,
                                           error);
            },
            [](const SipMessage &message) { return message.statusCode == 200; },
            std::chrono::seconds(3));
    require(spoofKeepaliveAck.has_value(),
            "GB28181 spoofed Keepalive was not acknowledged");
    const auto afterSpoofKeepalive = devices->findDevice("34020000001320000001");
    require(afterSpoofKeepalive.has_value() &&
                afterSpoofKeepalive->remoteAddress ==
                    "127.0.0.1:" + std::to_string(udp.local_endpoint().port()),
            "GB28181 Keepalive from an unexpected remote hijacked registration");

    const auto keepalive = sipKeepalive(udp.local_endpoint().port());
    udp.send_to(asio::buffer(keepalive), serverUdp);
    const auto keepaliveResponse =
        receiveUntil([&udp, &udpRemote](auto &buffer, std::error_code &error) {
          return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
        });
    require(keepaliveResponse.starts_with("SIP/2.0 200 OK"),
            "GB28181 keepalive was not acknowledged");

    udp.send_to(asio::buffer(catalogMessage("1x")), serverUdp);
    const auto badCatalogAck =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.statusCode == 200; },
            std::chrono::seconds(3));
    require(badCatalogAck.has_value(),
            "GB28181 malformed Catalog was not acknowledged");
    auto afterBadCatalog = devices->findDevice("34020000001320000001");
    require(afterBadCatalog.has_value() && afterBadCatalog->channels.size() == 1 &&
                afterBadCatalog->channels.front().ptzType == -1,
            "GB28181 Catalog accepted a non-decimal PTZType");

    udp.send_to(asio::buffer(catalogMessage("2")), serverUdp);
    const auto goodCatalogAck =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.statusCode == 200; },
            std::chrono::seconds(3));
    require(goodCatalogAck.has_value(),
            "GB28181 valid Catalog was not acknowledged");
    auto afterGoodCatalog = devices->findDevice("34020000001320000001");
    require(afterGoodCatalog.has_value() &&
                afterGoodCatalog->channels.size() == 1 &&
                afterGoodCatalog->channels.front().ptzType == 2,
            "GB28181 valid Catalog PTZType was not stored");

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

    asio::ip::tcp::socket oversizedHeaderTcp(clientContext);
    oversizedHeaderTcp.connect(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), sip.port));
    const std::string oversizedHeader(72 * 1024, 'A');
    asio::write(oversizedHeaderTcp, asio::buffer(oversizedHeader));
    require(socketClosesWithin(oversizedHeaderTcp, std::chrono::seconds(1)),
            "SIP TCP framing kept an oversized unterminated header open");

    asio::ip::tcp::socket invalidLengthTcp(clientContext);
    invalidLengthTcp.connect(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), sip.port));
    const auto invalidLengthRequest =
        "OPTIONS sip:34020000002000000001@127.0.0.1 SIP/2.0\r\n"
        "Via: SIP/2.0/TCP 127.0.0.1:" +
        std::to_string(invalidLengthTcp.local_endpoint().port()) +
        ";branch=z9hG4bK-invalid-length\r\n"
        "From: <sip:34020000001320000001@3402000000>;tag=test\r\n"
        "To: <sip:34020000002000000001@3402000000>\r\n"
        "Call-ID: gb28181-invalid-length-test\r\n"
        "CSeq: 2 OPTIONS\r\n"
        "Content-Length: -1\r\n\r\n";
    asio::write(invalidLengthTcp, asio::buffer(invalidLengthRequest));
    require(socketClosesWithin(invalidLengthTcp, std::chrono::seconds(1)),
            "SIP TCP framing kept an invalid Content-Length connection open");

    devices->updateCatalog("34020000001320000001",
                           {Channel{.id = "34020000001320000001",
                                    .name = "Main channel",
                                    .manufacturer = "test",
                                    .online = true,
                                    .ptzType = 1}});
    const auto preview =
        server->startPreview("34020000001320000001", "34020000001320000001");
    require(preview.has_value(), "GB28181 preview INVITE was not started");
    const auto invite =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.method == "INVITE"; },
            std::chrono::seconds(3));
    require(invite.has_value(), "GB28181 preview INVITE was not received");

    const auto notInviteOk = inviteOkWithCSeqMethod(*invite, "NOTINVITE");
    udp.send_to(asio::buffer(notInviteOk), serverUdp);
    const auto badAck =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.method == "ACK"; },
            std::chrono::milliseconds(300));
    require(!badAck.has_value(),
            "SIP accepted a 200 OK whose CSeq method was not INVITE");
    require(server->stopPreview(preview->sessionId).has_value(),
            "GB28181 pending preview session was not stopped");
    {
      std::lock_guard lock(viewerCountsMutex);
      require(viewerCounts.size() >= 2 &&
                  viewerCounts[viewerCounts.size() - 2] ==
                      std::pair{preview->streamId, 1U} &&
                  viewerCounts.back() == std::pair{preview->streamId, 0U},
              "GB28181 pending preview viewer count was not released");
      viewerCounts.clear();
    }
    const auto sharedPreview =
        server->startPreview("34020000001320000001", "34020000001320000001");
    require(sharedPreview.has_value(),
            "GB28181 shared preview INVITE was not started");
    const auto sharedInvite =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.method == "INVITE"; },
            std::chrono::seconds(3));
    require(sharedInvite.has_value(),
            "GB28181 shared preview INVITE was not received");
    const auto sharedInviteOk = inviteOkWithCSeqMethod(*sharedInvite, "INVITE");
    udp.send_to(asio::buffer(sharedInviteOk), serverUdp);
    const auto sharedAck =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.method == "ACK"; },
            std::chrono::seconds(3));
    require(sharedAck.has_value(),
            "GB28181 shared preview INVITE was not acknowledged");
    server->markStreamOnline(sharedPreview->streamId, true);

    const auto secondViewer =
        server->startPreview("34020000001320000001", "34020000001320000001");
    require(secondViewer.has_value() &&
                secondViewer->streamId == sharedPreview->streamId &&
                secondViewer->sessionId != sharedPreview->sessionId,
            "GB28181 concurrent preview did not reuse the camera stream");
    {
      std::unique_lock lock(viewerCountsMutex);
      require(viewerCountsChanged.wait_for(
                  lock, std::chrono::seconds(3),
                  [&viewerCounts] { return viewerCounts.size() >= 3; }),
              "GB28181 expired viewer lease did not reduce the viewer count");
      require(viewerCounts.back() == std::pair{sharedPreview->streamId, 1U},
              "GB28181 viewer lease expiry did not release exactly one viewer");
    }
    require(server->renewPreview(secondViewer->sessionId),
            "GB28181 active viewer lease could not be renewed");
    const auto prematureBye =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.method == "BYE"; },
            std::chrono::milliseconds(300));
    require(!prematureBye.has_value(),
            "GB28181 stream closed while another viewer was still active");
    require(server->stopPreview(secondViewer->sessionId).has_value(),
            "GB28181 second preview viewer was not released");
    const auto lastViewerBye =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.method == "BYE"; },
            std::chrono::seconds(3));
    require(lastViewerBye.has_value(),
            "GB28181 stream was not closed after its last viewer disappeared");
    require(!server->stopPreview(sharedPreview->sessionId).has_value(),
            "GB28181 preview session remained after its last viewer left");
    {
      std::lock_guard lock(viewerCountsMutex);
      require(viewerCounts ==
                  std::vector<std::pair<std::string, unsigned int>>{
                      {sharedPreview->streamId, 1U},
                      {sharedPreview->streamId, 2U},
                      {sharedPreview->streamId, 1U},
                      {sharedPreview->streamId, 0U},
                  },
              "GB28181 shared preview viewer count did not follow 1-2-1-0");
    }

    require(server->queryRecords("34020000001320000001",
                                 "34020000001320000001",
                                 "2026-07-29T08:40:45Z",
                                 "2026-07-29T08:41:45Z"),
            "GB28181 record query was not sent");
    const auto recordQuery =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) {
              return message.method == "MESSAGE" &&
                     xmlValue(message.body, "CmdType") == "RecordInfo";
            },
            std::chrono::seconds(3));
    require(recordQuery.has_value(), "GB28181 record query was not received");
    const auto recordSn = xmlValue(recordQuery->body, "SN");
    require(!recordSn.empty(), "GB28181 record query did not include SN");
    const auto badRecordInfo =
        recordInfoMessage(recordSn + "x", "spoofed-record-device");
    udp.send_to(asio::buffer(badRecordInfo), serverUdp);
    const auto badRecordAck =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.statusCode == 200; },
            std::chrono::seconds(3));
    require(badRecordAck.has_value(),
            "GB28181 malformed RecordInfo was not acknowledged");
    const auto afterBadRecord =
        devices->findDevice("34020000001320000001");
    require(afterBadRecord.has_value() && afterBadRecord->records.empty(),
            "GB28181 RecordInfo accepted a non-decimal SN for a pending query");

    const auto goodRecordInfo =
        recordInfoMessage(recordSn, "spoofed-record-device");
    udp.send_to(asio::buffer(goodRecordInfo), serverUdp);
    const auto goodRecordAck =
        receiveSipMatching(
            [&udp, &udpRemote](auto &buffer, std::error_code &error) {
              return udp.receive_from(asio::buffer(buffer), udpRemote, 0, error);
            },
            [](const SipMessage &message) { return message.statusCode == 200; },
            std::chrono::seconds(3));
    require(goodRecordAck.has_value(),
            "GB28181 valid RecordInfo was not acknowledged");
    const auto afterGoodRecord =
        devices->findDevice("34020000001320000001");
    require(afterGoodRecord.has_value() && afterGoodRecord->records.size() == 1,
            "GB28181 valid RecordInfo did not consume the pending query");

    devices->upsertRegistration(
        "bad-port-device",
        "127.0.0.1:" + std::to_string(udp.local_endpoint().port()) + "x");
    require(!server->queryCatalog("bad-port-device"),
            "GB28181 accepted a remote address with a non-decimal port");

    {
      DeviceRegistry ipv6Devices;
      const auto ipv6Address = asio::ip::address_v6::loopback();
      SipConfig ipv6Sip = sip;
      ipv6Sip.host = "::1";
      ipv6Sip.transport = "udp";
      ipv6Sip.port = reserveLocalUdpPort(ipv6Address);
      auto ipv6Server = std::make_shared<SipServer>(
          ipv6Sip, media, ipv6Devices, *zlm, loop);
      ipv6Server->start();

      asio::ip::udp::socket ipv6Client(
          clientContext, asio::ip::udp::endpoint(ipv6Address, 0));
      ipv6Client.non_blocking(true);
      asio::ip::udp::endpoint ipv6Remote;
      ipv6Devices.upsertRegistration(
          "ipv6-device",
          "[::1]:" + std::to_string(ipv6Client.local_endpoint().port()));
      require(ipv6Server->queryCatalog("ipv6-device"),
              "GB28181 IPv6 UDP route was not resolved");
      const auto ipv6Catalog =
          receiveSipMatching(
              [&ipv6Client, &ipv6Remote](auto &buffer,
                                         std::error_code &error) {
                return ipv6Client.receive_from(asio::buffer(buffer), ipv6Remote,
                                               0, error);
              },
              [](const SipMessage &message) {
                return message.method == "MESSAGE" &&
                       xmlValue(message.body, "CmdType") == "Catalog";
              },
              std::chrono::seconds(3));
      require(ipv6Catalog.has_value(),
              "GB28181 IPv6 UDP catalog query was not delivered");
      ipv6Server->stop();
    }

    verifyKeepaliveExpiry(server, sip, media, *devices, *zlm, loop,
                          clientContext);
    zlm->stop();
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
      server.reset();
      if (zlm)
        zlm->stop();
      zlm.reset();
      devices.reset();
      pool.stop();
      pool.join();
    } catch (...) {
    }
    std::cerr << error.what() << '\n';
    return 1;
  }
}
