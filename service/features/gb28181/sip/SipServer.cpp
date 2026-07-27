#include "sip/SipServer.h"

#include "sip/DigestAuth.h"
#include "sip/SipMessage.h"

#include <trantor/utils/Logger.h>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket invalidSocket = INVALID_SOCKET;
using SockLen = int;
#else
using NativeSocket = int;
constexpr NativeSocket invalidSocket = -1;
using SockLen = socklen_t;
#endif

struct TcpConnectionContext {
    std::string pending;
    SipServer::SipPeer peer;
};

struct RemoteEndpoint {
    std::string host;
    uint16_t port{0};
};

std::string extractUserFromSipUri(const std::string& value) {
    const auto sip = value.find("sip:");
    if (sip == std::string::npos) {
        return {};
    }
    const auto begin = sip + 4;
    const auto end = value.find_first_of("@;>", begin);
    if (end == std::string::npos) {
        return value.substr(begin);
    }
    return value.substr(begin, end - begin);
}

std::string extractTag(const std::string& value) {
    const auto tag = value.find("tag=");
    if (tag == std::string::npos) {
        return {};
    }
    const auto begin = tag + 4;
    const auto end = value.find_first_of(";> \t\r\n", begin);
    if (end == std::string::npos) {
        return value.substr(begin);
    }
    return value.substr(begin, end - begin);
}

unsigned int extractCSeqNumber(const std::string& value) {
    std::istringstream input(value);
    unsigned int cseq = 0;
    input >> cseq;
    return cseq;
}

std::string peerToString(const SipServer::SipPeer& peer) {
    return peer.address + ":" + std::to_string(peer.port);
}

const char* transportName(SipServer::SipTransport transport) {
    return transport == SipServer::SipTransport::Tcp ? "TCP" : "UDP";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<std::size_t> contentLengthOf(const std::string& headerText) {
    std::istringstream lines(headerText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (lower(line.substr(0, colon)) == "content-length") {
            try {
                return static_cast<std::size_t>(std::stoul(line.substr(colon + 1)));
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return 0;
}

std::string compactForLog(std::string value, std::size_t limit = 512) {
    const auto truncated = value.size() > limit;
    if (truncated) {
        value.resize(limit);
    }

    std::string output;
    output.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '\r') {
            output += "\\r";
        } else if (ch == '\n') {
            output += "\\n";
        } else if (ch == '\t') {
            output += "\\t";
        } else {
            output.push_back(ch);
        }
    }
    if (truncated) {
        output += "...";
    }
    return output;
}

std::optional<RemoteEndpoint> parseRemoteEndpoint(const std::string& remoteAddress) {
    const auto colon = remoteAddress.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= remoteAddress.size()) {
        return std::nullopt;
    }

    try {
        const auto port = std::stoi(remoteAddress.substr(colon + 1));
        if (port <= 0 || port > 65535) {
            return std::nullopt;
        }
        return RemoteEndpoint{
            remoteAddress.substr(0, colon),
            static_cast<uint16_t>(port),
        };
    } catch (...) {
        return std::nullopt;
    }
}

std::string routeUnavailableReason(const std::optional<DeviceRouteSnapshot>& route) {
    if (!route.has_value()) {
        return "device_not_registered";
    }
    if (!route->online) {
        return "device_offline";
    }
    if (route->remoteAddress.empty()) {
        return "remote_address_empty";
    }
    return {};
}

void logSipPacket(
    const char* direction,
    const SipMessage& message,
    const SipServer::SipPeer& remote,
    std::size_t bytes,
    bool includeBody
) {
    LOG_DEBUG << "[GB28181][SIP][" << direction << "] " << transportName(remote.transport)
              << " " << peerToString(remote)
              << ", start_line=\"" << message.startLine << "\""
              << ", call_id=" << message.header("Call-ID")
              << ", cseq=\"" << message.header("CSeq") << "\""
              << ", bytes=" << bytes
              << ", body_bytes=" << message.body.size();

    if (includeBody && !message.body.empty()) {
        LOG_TRACE << "[GB28181][SIP][" << direction << "_BODY] " << transportName(remote.transport)
                  << " " << peerToString(remote)
                  << ", call_id=" << message.header("Call-ID")
                  << ", body=\"" << compactForLog(message.body, 2048) << "\"";
    }
}

void logSipSend(const std::string& packet, const SipServer::SipPeer& remote, bool includeBody) {
    const auto message = SipMessage::parse(packet);
    if (!message.has_value()) {
        LOG_DEBUG << "[GB28181][SIP][TX] " << transportName(remote.transport)
                  << " " << peerToString(remote)
                  << ", bytes=" << packet.size()
                  << ", first_bytes=\"" << compactForLog(packet, 160) << "\"";
        return;
    }
    logSipPacket("TX", *message, remote, packet.size(), includeBody);
}

std::string xmlText(const pugi::xml_node& node, const char* name) {
    return node.child(name).text().as_string();
}

int xmlInt(const pugi::xml_node& node, const char* name, int fallback = -1) {
    auto text = xmlText(node, name);
    if (text.empty()) {
        text = xmlText(node.child("Info"), name);
    }
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

bool statusOnline(const std::string& status) {
    return status == "ON" || status == "ONLINE" || status == "OK";
}

std::string makeToken(const char* prefix) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream output;
    output << prefix << std::chrono::steady_clock::now().time_since_epoch().count() << rng();
    return output.str();
}

std::string ptzCommand(const std::string& action, uint8_t speed) {
    const auto clamped = static_cast<uint8_t>(std::min<unsigned int>(speed, 255));
    uint8_t command = 0x00;
    uint8_t panSpeed = 0x00;
    uint8_t tiltSpeed = 0x00;
    uint8_t zoomSpeed = 0x00;

    if (action == "left") {
        command = 0x02;
        panSpeed = clamped;
    } else if (action == "right") {
        command = 0x01;
        panSpeed = clamped;
    } else if (action == "up") {
        command = 0x08;
        tiltSpeed = clamped;
    } else if (action == "down") {
        command = 0x04;
        tiltSpeed = clamped;
    } else if (action == "zoomin") {
        command = 0x10;
        zoomSpeed = clamped;
    } else if (action == "zoomout") {
        command = 0x20;
        zoomSpeed = clamped;
    }

    std::array<uint8_t, 8> bytes{0xA5, 0x0F, 0x01, command, panSpeed, tiltSpeed, zoomSpeed, 0x00};
    unsigned int checksum = 0;
    for (std::size_t i = 0; i < bytes.size() - 1; ++i) {
        checksum += bytes[i];
    }
    bytes.back() = static_cast<uint8_t>(checksum & 0xFF);

    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

long long gbTimeToUnixSeconds(const std::string& value) {
    std::tm tm{};
    std::istringstream input(value);
    input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) {
        input.clear();
        input.str(value);
        input >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    }
    if (input.fail()) {
        return 0;
    }
#ifdef _WIN32
    return static_cast<long long>(_mkgmtime(&tm));
#else
    return static_cast<long long>(timegm(&tm));
#endif
}

bool isInvalidSocket(NativeSocket socket) {
    return socket == invalidSocket;
}

void closeSocket(NativeSocket socket) {
    if (isInvalidSocket(socket)) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

std::string socketErrorMessage() {
#ifdef _WIN32
    return "WSA error=" + std::to_string(WSAGetLastError());
#else
    return "errno=" + std::to_string(errno);
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    const auto error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void setNonBlocking(NativeSocket socket) {
#ifdef _WIN32
    u_long mode = 1;
    if (::ioctlsocket(socket, FIONBIO, &mode) != 0) {
        throw std::runtime_error("Could not set UDP socket non-blocking: " + socketErrorMessage());
    }
#else
    const auto flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("Could not set UDP socket non-blocking: " + socketErrorMessage());
    }
#endif
}

sockaddr_in makeSocketAddress(const std::string& host, uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("Invalid IPv4 address: " + host);
    }
    return address;
}

std::string socketAddressToIp(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return buffer;
}

class LoopDispatchAwaiter {
public:
    using Work = std::function<void()>;

    LoopDispatchAwaiter(trantor::EventLoop* loop, Work work)
        : loop_(loop), work_(std::move(work)) {}

    bool await_ready() const noexcept {
        return loop_ == nullptr || loop_->isInLoopThread();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        resumeLoop_ = trantor::EventLoop::getEventLoopOfCurrentThread();
        loop_->queueInLoop([this, handle]() mutable {
            runWork();
            // Network socket work runs on TcpIoPool, but the awaiting startup
            // coroutine must continue on its original loop so later DB/HTTP
            // work stays in the Drogon domain.
            if (resumeLoop_ != nullptr && resumeLoop_ != loop_) {
                resumeLoop_->queueInLoop([handle]() mutable {
                    handle.resume();
                });
                return;
            }
            handle.resume();
        });
    }

    void await_resume() {
        if (await_ready()) {
            runWork();
        }
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

private:
    void runWork() {
        if (ran_) {
            return;
        }
        ran_ = true;
        try {
            work_();
        } catch (...) {
            exception_ = std::current_exception();
        }
    }

    trantor::EventLoop* loop_{nullptr};
    trantor::EventLoop* resumeLoop_{nullptr};
    Work work_;
    bool ran_{false};
    std::exception_ptr exception_;
};

} // namespace

SipServer::SipServer(SipConfig sipConfig, MediaConfig mediaConfig, DeviceRegistry& deviceRegistry,
                     ZlmClient& zlmClient, trantor::EventLoop* ioLoop)
    : sipConfig_(std::move(sipConfig)),
      mediaConfig_(std::move(mediaConfig)),
      deviceRegistry_(deviceRegistry),
      zlmClient_(zlmClient),
      ioLoop_(ioLoop) {}

SipServer::~SipServer() {
    if (!running_.load()) {
        return;
    }
    if (ioLoop_ != nullptr && ioLoop_->isInLoopThread()) {
        running_.store(false);
        stopInLoop();
    } else {
        LOG_WARN << "[GB28181][SIP] SipServer destroyed while running; call stopCoro() before destruction";
    }
}

void SipServer::start() {
    drogon::async_run([this]() -> drogon::Task<> {
        co_await startCoro();
    });
}

drogon::Task<> SipServer::startCoro() {
    if (running_.exchange(true)) {
        LOG_DEBUG << "[GB28181][SIP] Start skipped: server already running";
        co_return;
    }

    LOG_DEBUG << "[GB28181][SIP] Starting, listen=" << sipConfig_.host << ":" << sipConfig_.port
             << ", domain=" << sipConfig_.domain
             << ", sip_id=" << sipConfig_.id
             << ", public_ip=" << sipConfig_.publicIp
             << ", media_rtp_ip=" << mediaConfig_.rtpPublicIp
             << ", rtp_port_range=" << mediaConfig_.rtpPortRangeStart << "-" << mediaConfig_.rtpPortRangeEnd
             << ", zlm_base_url=" << mediaConfig_.zlmBaseUrl;

    if (ioLoop_ == nullptr) {
        running_.store(false);
        throw std::runtime_error("GB28181 SIP IO loop is not configured");
    }
    auto startWork = [this]() {
        try {
            startInLoop();
        } catch (...) {
            running_.store(false);
            throw;
        }
    };
    co_await LoopDispatchAwaiter(ioLoop_, std::move(startWork));
}

void SipServer::stop() {
    drogon::async_run([this]() -> drogon::Task<> {
        co_await stopCoro();
    });
}

drogon::Task<> SipServer::stopCoro() {
    if (!running_.exchange(false)) {
        LOG_DEBUG << "[GB28181][SIP] Stop skipped: server is not running";
        co_return;
    }

    if (ioLoop_ == nullptr) {
        LOG_DEBUG << "[GB28181][SIP] Stop skipped: IO loop is not available";
        co_return;
    }
    LOG_DEBUG << "[GB28181][SIP] Stopping";
    co_await LoopDispatchAwaiter(ioLoop_, [this]() {
        stopInLoop();
    });
}

void SipServer::startInLoop() {
    if (ioLoop_ == nullptr) {
        throw std::runtime_error("GB28181 SIP IO loop is not available");
    }

    udpSocket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (isInvalidSocket(udpSocket_)) {
        throw std::runtime_error("Could not create GB28181 UDP socket: " + socketErrorMessage());
    }
    int reuse = 1;
    ::setsockopt(udpSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    setNonBlocking(udpSocket_);

    const auto udpAddress = makeSocketAddress(sipConfig_.host, sipConfig_.port);
    if (::bind(udpSocket_, reinterpret_cast<const sockaddr*>(&udpAddress), sizeof(udpAddress)) != 0) {
        const auto message = socketErrorMessage();
        closeSocket(udpSocket_);
        udpSocket_ = invalidSocket;
        throw std::runtime_error("Could not bind GB28181 UDP socket: " + message);
    }

    udpChannel_ = std::make_unique<trantor::Channel>(ioLoop_, static_cast<int>(udpSocket_));
    udpChannel_->setReadCallback([this]() { handleUdpReadable(); });
    udpChannel_->enableReading();

    const auto tcpAddress = trantor::InetAddress(sipConfig_.host, sipConfig_.port);
#ifdef _WIN32
    tcpServer_ = std::make_shared<trantor::TcpServer>(ioLoop_, tcpAddress, "Gb28181SipTcpServer", true, false);
#else
    tcpServer_ = std::make_shared<trantor::TcpServer>(ioLoop_, tcpAddress, "Gb28181SipTcpServer");
#endif
    tcpServer_->setConnectionCallback([this](const trantor::TcpConnectionPtr& connection) {
        handleTcpConnection(connection);
    });
    tcpServer_->setRecvMessageCallback([this](const trantor::TcpConnectionPtr& connection, trantor::MsgBuffer* buffer) {
        handleTcpMessage(connection, buffer);
    });
    tcpServer_->start();

    LOG_DEBUG << "[GB28181][SIP] UDP listening on " << sipConfig_.host << ":" << sipConfig_.port << " via TcpIoPool";
    LOG_DEBUG << "[GB28181][SIP] TCP listening on " << sipConfig_.host << ":" << sipConfig_.port << " via TcpIoPool";
}

void SipServer::stopInLoop() {
    std::size_t sessionCount = 0;
    std::size_t viewerCount = 0;
    {
        std::lock_guard lock(sessionMutex_);
        sessionCount = previewSessions_.size();
        viewerCount = previewViewers_.size();
    }

    if (udpChannel_) {
        udpChannel_->disableAll();
        udpChannel_->remove();
        udpChannel_.reset();
    }
    closeSocket(udpSocket_);
    udpSocket_ = invalidSocket;

    if (tcpServer_) {
        tcpServer_->stop();
        tcpServer_.reset();
    }

    std::size_t tcpConnectionCount = 0;
    {
        std::lock_guard lock(tcpConnectionsMutex_);
        tcpConnectionCount = tcpConnections_.size();
        for (auto& [_, connection] : tcpConnections_) {
            if (connection && connection->connected()) {
                connection->forceClose();
            }
        }
        tcpConnections_.clear();
    }

    LOG_DEBUG << "[GB28181][SIP] Stopped, active_sessions=" << sessionCount
             << ", active_viewers=" << viewerCount
             << ", tcp_connections=" << tcpConnectionCount;
}

void SipServer::handleUdpReadable() {
    while (running_) {
        std::array<char, 8192> buffer{};
        sockaddr_in remoteAddress{};
        SockLen remoteLength = sizeof(remoteAddress);
        const auto size = ::recvfrom(
            udpSocket_,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&remoteAddress),
            &remoteLength);
        if (size < 0) {
            if (!wouldBlock() && running_) {
                LOG_WARN << "[GB28181][SIP] UDP receive failed: " << socketErrorMessage();
            }
            return;
        }

        SipPeer peer;
        peer.transport = SipTransport::Udp;
        peer.udp = remoteAddress;
        peer.address = socketAddressToIp(remoteAddress);
        peer.port = ntohs(remoteAddress.sin_port);
        if (sipConfig_.logging) {
            LOG_DEBUG << "[GB28181][SIP][UDP_RX] remote=" << peerToString(peer)
                      << ", bytes=" << size;
        }
        handlePacket(std::string(buffer.data(), static_cast<size_t>(size)), peer);
    }
}

void SipServer::handleTcpConnection(const trantor::TcpConnectionPtr& connection) {
    const auto key = connection->peerAddr().toIpPort();
    if (connection->connected()) {
        SipPeer peer;
        peer.transport = SipTransport::Tcp;
        peer.tcp = connection;
        peer.address = connection->peerAddr().toIp();
        peer.port = connection->peerAddr().toPort();

        auto context = std::make_shared<TcpConnectionContext>();
        context->peer = peer;
        connection->setContext(context);

        {
            std::lock_guard lock(tcpConnectionsMutex_);
            tcpConnections_[key] = connection;
        }
        LOG_DEBUG << "[GB28181][SIP] TCP connected from " << key;
        return;
    }

    {
        std::lock_guard lock(tcpConnectionsMutex_);
        const auto iter = tcpConnections_.find(key);
        if (iter != tcpConnections_.end() && iter->second == connection) {
            tcpConnections_.erase(iter);
        }
    }
    connection->clearContext();
    LOG_DEBUG << "[GB28181][SIP] TCP disconnected from " << key;
}

void SipServer::handleTcpMessage(const trantor::TcpConnectionPtr& connection, trantor::MsgBuffer* buffer) {
    auto context = connection->getContext<TcpConnectionContext>();
    if (!context) {
        return;
    }

    context->pending.append(buffer->peek(), buffer->readableBytes());
    buffer->retrieveAll();

    const auto key = connection->peerAddr().toIpPort();
    if (sipConfig_.logging) {
        LOG_DEBUG << "[GB28181][SIP][TCP_RX] remote=" << key
                  << ", pending_bytes=" << context->pending.size();
    }
    while (true) {
        const auto headerEnd = context->pending.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            break;
        }
        const auto contentLength = contentLengthOf(context->pending.substr(0, headerEnd));
        if (!contentLength.has_value()) {
            LOG_WARN << "[GB28181][SIP] TCP message with invalid Content-Length from " << key
                     << ", header=\"" << compactForLog(context->pending.substr(0, headerEnd), 300) << "\"";
            context->pending.clear();
            break;
        }
        const auto packetSize = headerEnd + 4 + *contentLength;
        if (context->pending.size() < packetSize) {
            break;
        }
        auto packet = context->pending.substr(0, packetSize);
        context->pending.erase(0, packetSize);
        handlePacket(packet, context->peer);
    }
}

void SipServer::handlePacket(const std::string& packet, const SipPeer& remote) {
    const auto message = SipMessage::parse(packet);
    if (!message.has_value()) {
        LOG_WARN << "[GB28181][SIP] Ignored malformed packet from " << transportName(remote.transport)
                 << " " << peerToString(remote)
                 << ", bytes=" << packet.size()
                 << ", first_bytes=\"" << compactForLog(packet, 200) << "\"";
        return;
    }

    if (sipConfig_.logging) {
        logSipPacket("RX", *message, remote, packet.size(), true);
    }

    if (message->statusCode > 0) {
        handleResponse(*message, remote);
        return;
    }

    if (message->method == "REGISTER") {
        handleRegister(*message, remote);
        return;
    }

    if (message->method == "MESSAGE") {
        handleMessage(*message, remote);
        return;
    }

    if (sipConfig_.logging) {
        LOG_WARN << "[GB28181][SIP] Unsupported method, method=" << message->method
                 << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
                 << ", call_id=" << message->header("Call-ID")
                 << ", cseq=\"" << message->header("CSeq") << "\"";
    }
    sendResponse(*message, remote, 405, "Method Not Allowed");
}

void SipServer::handleResponse(const SipMessage& message, const SipPeer& remote) {
    const auto cseq = message.header("CSeq");
    if (message.statusCode == 200 && cseq.find("INVITE") != std::string::npos) {
        handleInviteOk(message, remote);
        return;
    }

    if (message.statusCode >= 300) {
        LOG_WARN << "[GB28181][SIP] Error response, status=" << message.statusCode
                 << ", reason=\"" << message.reasonPhrase << "\""
                 << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
                 << ", call_id=" << message.header("Call-ID")
                 << ", cseq=\"" << cseq << "\""
                 << ", body=\"" << compactForLog(message.body, 300) << "\"";
        return;
    }

    LOG_DEBUG << "[GB28181][SIP] Unhandled response, status=" << message.statusCode
              << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
              << ", call_id=" << message.header("Call-ID")
              << ", cseq=\"" << cseq << "\"";
}

void SipServer::handleInviteOk(const SipMessage& message, const SipPeer& remote) {
    const auto callId = message.header("Call-ID");
    if (callId.empty()) {
        LOG_WARN << "[GB28181][Invite] 200 OK without Call-ID from "
                 << transportName(remote.transport) << " " << peerToString(remote);
        return;
    }

    PreviewSession session;
    bool found = false;
    {
        std::lock_guard lock(sessionMutex_);
        for (auto& [_, candidate] : previewSessions_) {
            if (candidate.callId == callId) {
                session = candidate;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        LOG_WARN << "[GB28181][Invite] 200 OK for unknown Call-ID, call_id=" << callId
                 << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
                 << ", cseq=\"" << message.header("CSeq") << "\"";
        return;
    }

    const auto to = message.header("To");
    const auto cseq = extractCSeqNumber(message.header("CSeq"));
    const auto toTag = extractTag(to);
    if (!message.body.empty()) {
        LOG_DEBUG << "[GB28181][Invite] 200 OK SDP received, session=" << session.sessionId
                 << ", mode=" << session.mode
                 << ", device=" << session.deviceId
                 << ", channel=" << session.channelId
                 << ", stream_id=" << session.streamId
                 << ", call_id=" << callId
                 << ", body=\"" << compactForLog(message.body, 1200) << "\"";
    }
    {
        std::lock_guard lock(sessionMutex_);
        for (auto& [_, candidate] : previewSessions_) {
            if (candidate.callId == callId) {
                candidate.toTag = toTag;
                candidate.established = true;
                break;
            }
        }
    }

    const auto host = remote.address;
    const auto port = remote.port;
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("ack");
    const auto transport = transportName(remote.transport);

    std::ostringstream ack;
    ack << "ACK sip:" << session.channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
        << "Via: SIP/2.0/" << transport << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
        << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << session.fromTag << "\r\n"
        << "To: " << to << "\r\n"
        << "Call-ID: " << callId << "\r\n"
        << "CSeq: " << cseq << " ACK\r\n"
        << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
        << "Max-Forwards: 70\r\n"
        << "User-Agent: gb28181-platform-cpp\r\n"
        << "Content-Length: 0\r\n\r\n";

    sendRequest(ack.str(), remote);
    LOG_DEBUG << "[GB28181][Invite] ACK sent, session=" << session.sessionId
             << ", mode=" << session.mode
             << ", device=" << session.deviceId
             << ", channel=" << session.channelId
             << ", stream_id=" << session.streamId
             << ", call_id=" << callId
             << ", cseq=" << cseq
             << ", to_tag=" << toTag
             << ", remote=" << transportName(remote.transport) << " " << peerToString(remote);
}

void SipServer::handleRegister(const SipMessage& message, const SipPeer& remote) {
    if (!DigestAuth::verifyRegister(message, sipConfig_.domain, sipConfig_.password)) {
        const auto nonce = DigestAuth::makeNonce();
        std::ostringstream auth;
        auth << "WWW-Authenticate: Digest realm=\"" << sipConfig_.domain
             << "\", nonce=\"" << nonce
             << "\", algorithm=MD5, qop=\"auth\"\r\n";
        sendResponse(message, remote, 401, "Unauthorized", auth.str());
        if (sipConfig_.logging) {
            LOG_DEBUG << "[GB28181][Register] Auth challenge sent, device_hint="
                     << extractUserFromSipUri(message.header("From"))
                     << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
                     << ", call_id=" << message.header("Call-ID")
                     << ", cseq=\"" << message.header("CSeq") << "\"";
        }
        return;
    }

    auto deviceId = extractUserFromSipUri(message.header("From"));
    if (deviceId.empty()) {
        deviceId = extractUserFromSipUri(message.header("Contact"));
    }
    if (deviceId.empty()) {
        deviceId = remote.address;
    }

    deviceRegistry_.upsertRegistration(deviceId, peerToString(remote));
    LOG_INFO << "[GB28181][Register] Device registered, device=" << deviceId
             << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
             << ", contact=\"" << message.header("Contact") << "\""
             << ", expires=\"" << message.header("Expires") << "\""
             << ", call_id=" << message.header("Call-ID")
             << ", cseq=\"" << message.header("CSeq") << "\"";

    sendResponse(message, remote, 200, "OK");

    scheduleCatalogQuery(deviceId);
}

void SipServer::handleMessage(const SipMessage& message, const SipPeer& remote) {
    sendResponse(message, remote, 200, "OK");

    pugi::xml_document document;
    const auto result = document.load_string(message.body.c_str());
    if (!result) {
        LOG_WARN << "[GB28181][Message] Ignored invalid XML from " << transportName(remote.transport)
                 << " " << peerToString(remote)
                 << ", error=" << result.description()
                 << ", body=\"" << compactForLog(message.body, 500) << "\"";
        return;
    }

    auto root = document.first_child();
    const auto cmdType = xmlText(root, "CmdType");
    auto deviceId = xmlText(root, "DeviceID");
    if (deviceId.empty()) {
        LOG_WARN << "[GB28181][Message] Ignored MESSAGE without DeviceID, cmd_type=" << cmdType
                 << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
                 << ", body=\"" << compactForLog(message.body, 500) << "\"";
        return;
    }
    const auto snText = xmlText(root, "SN");
    LOG_DEBUG << "[GB28181][Message] Received, cmd_type=" << cmdType
             << ", device=" << deviceId
             << ", sn=" << snText
             << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
             << ", body_bytes=" << message.body.size();

    if (cmdType == "Keepalive") {
        const auto status = xmlText(root, "Status");
        bool shouldQueryCatalog = false;
        if (statusOnline(status)) {
            shouldQueryCatalog = deviceRegistry_.updateKeepaliveAndNeedsCatalog(deviceId, peerToString(remote));
            if (shouldQueryCatalog) {
                scheduleCatalogQuery(deviceId);
            }
        } else {
            deviceRegistry_.markOffline(deviceId);
        }
        LOG_DEBUG << "[GB28181][Keepalive] device=" << deviceId
                 << ", status=" << status
                 << ", online=" << statusOnline(status)
                 << ", catalog_scheduled=" << shouldQueryCatalog
                 << ", remote=" << transportName(remote.transport) << " " << peerToString(remote);
        return;
    }

    if (cmdType == "Catalog") {
        std::vector<Channel> channels;
        std::size_t onlineCount = 0;
        for (auto item : root.child("DeviceList").children("Item")) {
            Channel channel;
            channel.id = xmlText(item, "DeviceID");
            channel.name = xmlText(item, "Name");
            channel.manufacturer = xmlText(item, "Manufacturer");
            channel.online = statusOnline(xmlText(item, "Status"));
            channel.ptzType = xmlInt(item, "PTZType");
            if (!channel.id.empty()) {
                if (channel.online) {
                    ++onlineCount;
                }
                LOG_DEBUG << "[GB28181][Catalog] Channel, device=" << deviceId
                          << ", channel=" << channel.id
                          << ", name=\"" << channel.name << "\""
                          << ", manufacturer=\"" << channel.manufacturer << "\""
                          << ", online=" << channel.online
                          << ", ptz_type=" << channel.ptzType;
                channels.push_back(std::move(channel));
            }
        }
        const auto channelCount = channels.size();
        deviceRegistry_.updateCatalog(deviceId, std::move(channels));
        LOG_DEBUG << "[GB28181][Catalog] Updated, device=" << deviceId
                 << ", sn=" << snText
                 << ", channels=" << channelCount
                 << ", online_channels=" << onlineCount
                 << ", remote=" << transportName(remote.transport) << " " << peerToString(remote);
        return;
    }

    if (cmdType == "RecordInfo") {
        auto originalDeviceId = deviceId;
        if (!snText.empty()) {
            try {
                const auto sn = static_cast<unsigned int>(std::stoul(snText));
                std::lock_guard lock(sessionMutex_);
                const auto iter = pendingRecordQueries_.find(sn);
                if (iter != pendingRecordQueries_.end()) {
                    deviceId = iter->second;
                    pendingRecordQueries_.erase(iter);
                }
            } catch (...) {
            }
        }
        std::vector<RecordItem> records;
        for (auto item : root.child("RecordList").children("Item")) {
            RecordItem record;
            record.deviceId = xmlText(item, "DeviceID");
            record.name = xmlText(item, "Name");
            record.filePath = xmlText(item, "FilePath");
            record.address = xmlText(item, "Address");
            record.startTime = xmlText(item, "StartTime");
            record.endTime = xmlText(item, "EndTime");
            record.type = xmlText(item, "Type");
            record.recorderId = xmlText(item, "RecorderID");
            if (!record.deviceId.empty()) {
                LOG_DEBUG << "[GB28181][Record] Item, device=" << deviceId
                          << ", channel=" << record.deviceId
                          << ", name=\"" << record.name << "\""
                          << ", start_time=" << record.startTime
                          << ", end_time=" << record.endTime
                          << ", type=" << record.type;
                records.push_back(std::move(record));
            }
        }
        const auto recordCount = records.size();
        deviceRegistry_.updateRecords(deviceId, std::move(records));
        LOG_DEBUG << "[GB28181][Record] Updated, device=" << deviceId
                 << ", reported_device=" << originalDeviceId
                 << ", sn=" << snText
                 << ", records=" << recordCount
                 << ", remote=" << transportName(remote.transport) << " " << peerToString(remote);
        return;
    }

    LOG_WARN << "[GB28181][Message] Unhandled CmdType, cmd_type=" << cmdType
             << ", device=" << deviceId
             << ", sn=" << snText
             << ", remote=" << transportName(remote.transport) << " " << peerToString(remote);
}

bool SipServer::queryCatalog(const std::string& deviceId) {
    LOG_DEBUG << "[GB28181][Catalog] Query requested, device=" << deviceId;

    const auto route = deviceRegistry_.findRouteSnapshot(deviceId);
    const auto unavailableReason = routeUnavailableReason(route);
    if (!unavailableReason.empty()) {
        LOG_WARN << "[GB28181][Catalog] Query skipped, device=" << deviceId
                 << ", reason=" << unavailableReason;
        return false;
    }

    const auto endpoint = parseRemoteEndpoint(route->remoteAddress);
    if (!endpoint.has_value()) {
        LOG_WARN << "[GB28181][Catalog] Query skipped, device=" << deviceId
                 << ", reason=invalid_remote_address"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    auto remote = peerFromAddress(route->remoteAddress);
    if (!remote.has_value()) {
        LOG_WARN << "[GB28181][Catalog] Query skipped, device=" << deviceId
                 << ", reason=peer_unavailable"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    const auto sn = cseq_.fetch_add(1);
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
         << "<Query>\r\n"
         << "<CmdType>Catalog</CmdType>\r\n"
         << "<SN>" << sn << "</SN>\r\n"
         << "<DeviceID>" << deviceId << "</DeviceID>\r\n"
         << "</Query>\r\n";

    const auto bodyText = body.str();
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("branch");
    const auto tag = makeToken("tag");
    const auto callId = makeToken("catalog") + "@" + sipConfig_.domain;

    std::ostringstream request;
    request << "MESSAGE sip:" << deviceId << "@" << endpoint->host << ":" << endpoint->port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << tag << "\r\n"
            << "To: <sip:" << deviceId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << sn << " MESSAGE\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: Application/MANSCDP+xml\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    sendRequest(request.str(), *remote);
    LOG_DEBUG << "[GB28181][Catalog] Query sent, device=" << deviceId
             << ", sn=" << sn
             << ", call_id=" << callId
             << ", branch=" << branch
             << ", remote=" << transportName(remote->transport) << " " << peerToString(*remote)
             << ", body_bytes=" << bodyText.size();
    return true;
}

bool SipServer::queryRecords(const std::string& deviceId, const std::string& channelId, const std::string& startTime, const std::string& endTime) {
    LOG_DEBUG << "[GB28181][Record] Query requested, device=" << deviceId
             << ", channel=" << channelId
             << ", start_time=" << startTime
             << ", end_time=" << endTime;

    const auto route = deviceRegistry_.findRouteSnapshot(deviceId);
    const auto unavailableReason = routeUnavailableReason(route);
    if (!unavailableReason.empty()) {
        LOG_WARN << "[GB28181][Record] Query skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=" << unavailableReason;
        return false;
    }

    const auto endpoint = parseRemoteEndpoint(route->remoteAddress);
    if (!endpoint.has_value()) {
        LOG_WARN << "[GB28181][Record] Query skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=invalid_remote_address"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    auto remote = peerFromAddress(route->remoteAddress);
    if (!remote.has_value()) {
        LOG_WARN << "[GB28181][Record] Query skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=peer_unavailable"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    const auto sn = cseq_.fetch_add(1);
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
         << "<Query>\r\n"
         << "<CmdType>RecordInfo</CmdType>\r\n"
         << "<SN>" << sn << "</SN>\r\n"
         << "<DeviceID>" << channelId << "</DeviceID>\r\n"
         << "<StartTime>" << startTime << "</StartTime>\r\n"
         << "<EndTime>" << endTime << "</EndTime>\r\n"
         << "<Secrecy>0</Secrecy>\r\n"
         << "<Type>all</Type>\r\n"
         << "</Query>\r\n";

    const auto bodyText = body.str();
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("record");
    const auto tag = makeToken("tag");
    const auto callId = makeToken("record") + "@" + sipConfig_.domain;
    {
        std::lock_guard lock(sessionMutex_);
        pendingRecordQueries_[sn] = deviceId;
    }

    std::ostringstream request;
    request << "MESSAGE sip:" << channelId << "@" << endpoint->host << ":" << endpoint->port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << tag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << sn << " MESSAGE\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: Application/MANSCDP+xml\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    sendRequest(request.str(), *remote);
    LOG_DEBUG << "[GB28181][Record] Query sent, device=" << deviceId
             << ", channel=" << channelId
             << ", sn=" << sn
             << ", call_id=" << callId
             << ", branch=" << branch
             << ", remote=" << transportName(remote->transport) << " " << peerToString(*remote)
             << ", body_bytes=" << bodyText.size();
    return true;
}

bool SipServer::sendPtzControl(const std::string& deviceId, const std::string& channelId, const std::string& action, uint8_t speed) {
    LOG_DEBUG << "[GB28181][PTZ] Control requested, device=" << deviceId
             << ", channel=" << channelId
             << ", action=" << action
             << ", speed=" << static_cast<unsigned int>(speed);

    const auto route = deviceRegistry_.findRouteSnapshot(deviceId);
    const auto unavailableReason = routeUnavailableReason(route);
    if (!unavailableReason.empty()) {
        LOG_WARN << "[GB28181][PTZ] Control skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", action=" << action
                 << ", reason=" << unavailableReason;
        return false;
    }

    const auto endpoint = parseRemoteEndpoint(route->remoteAddress);
    if (!endpoint.has_value()) {
        LOG_WARN << "[GB28181][PTZ] Control skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", action=" << action
                 << ", reason=invalid_remote_address"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    auto remote = peerFromAddress(route->remoteAddress);
    if (!remote.has_value()) {
        LOG_WARN << "[GB28181][PTZ] Control skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", action=" << action
                 << ", reason=peer_unavailable"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    const auto sn = cseq_.fetch_add(1);
    const auto command = ptzCommand(action, speed);
    std::ostringstream body;
    body << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
         << "<Control>\r\n"
         << "<CmdType>DeviceControl</CmdType>\r\n"
         << "<SN>" << sn << "</SN>\r\n"
         << "<DeviceID>" << channelId << "</DeviceID>\r\n"
         << "<PTZCmd>" << command << "</PTZCmd>\r\n"
         << "<Info>\r\n"
         << "<ControlPriority>5</ControlPriority>\r\n"
         << "</Info>\r\n"
         << "</Control>\r\n";

    const auto bodyText = body.str();
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("ptz");
    const auto tag = makeToken("tag");
    const auto callId = makeToken("ptz") + "@" + sipConfig_.domain;

    std::ostringstream request;
    request << "MESSAGE sip:" << channelId << "@" << endpoint->host << ":" << endpoint->port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << tag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << sn << " MESSAGE\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: Application/MANSCDP+xml\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    sendRequest(request.str(), *remote);
    LOG_DEBUG << "[GB28181][PTZ] Control sent, device=" << deviceId
             << ", channel=" << channelId
             << ", action=" << action
             << ", speed=" << static_cast<unsigned int>(speed)
             << ", command=" << command
             << ", sn=" << sn
             << ", call_id=" << callId
             << ", branch=" << branch
             << ", remote=" << transportName(remote->transport) << " " << peerToString(*remote)
             << ", body_bytes=" << bodyText.size();
    return true;
}

bool SipServer::sendPtzPreciseControl(
    const std::string& deviceId,
    const std::string& channelId,
    double pan,
    double tilt,
    double zoom) {
    LOG_DEBUG << "[GB28181][PTZ] Precise control requested, device=" << deviceId
              << ", channel=" << channelId
              << ", pan=" << pan
              << ", tilt=" << tilt
              << ", zoom=" << zoom;

    const auto route = deviceRegistry_.findRouteSnapshot(deviceId);
    const auto unavailableReason = routeUnavailableReason(route);
    if (!unavailableReason.empty()) {
        LOG_WARN << "[GB28181][PTZ] Precise control skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=" << unavailableReason;
        return false;
    }

    const auto endpoint = parseRemoteEndpoint(route->remoteAddress);
    if (!endpoint.has_value()) {
        LOG_WARN << "[GB28181][PTZ] Precise control skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=invalid_remote_address"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    auto remote = peerFromAddress(route->remoteAddress);
    if (!remote.has_value()) {
        LOG_WARN << "[GB28181][PTZ] Precise control skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=peer_unavailable"
                 << ", remote_address=" << route->remoteAddress;
        return false;
    }

    const auto sn = cseq_.fetch_add(1);
    std::ostringstream body;
    body.imbue(std::locale::classic());
    body << std::fixed << std::setprecision(2)
         << "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
         << "<Control>\r\n"
         << "<CmdType>DeviceControl</CmdType>\r\n"
         << "<SN>" << sn << "</SN>\r\n"
         << "<DeviceID>" << channelId << "</DeviceID>\r\n"
         << "<PTZPreciseCtrl>\r\n"
         << "<Pan>" << pan << "</Pan>\r\n"
         << "<Tilt>" << tilt << "</Tilt>\r\n"
         << "<Zoom>" << zoom << "</Zoom>\r\n"
         << "</PTZPreciseCtrl>\r\n"
         << "</Control>\r\n";

    const auto bodyText = body.str();
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto branch = "z9hG4bK-" + makeToken("ptz-precise");
    const auto tag = makeToken("tag");
    const auto callId = makeToken("ptz-precise") + "@" + sipConfig_.domain;

    std::ostringstream request;
    request << "MESSAGE sip:" << channelId << "@" << endpoint->host << ":" << endpoint->port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << tag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << sn << " MESSAGE\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: Application/MANSCDP+xml\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    sendRequest(request.str(), *remote);
    LOG_DEBUG << "[GB28181][PTZ] Precise control sent, device=" << deviceId
              << ", channel=" << channelId
              << ", pan=" << pan
              << ", tilt=" << tilt
              << ", zoom=" << zoom
              << ", sn=" << sn
              << ", call_id=" << callId
              << ", branch=" << branch
              << ", remote=" << transportName(remote->transport) << " " << peerToString(*remote)
              << ", body_bytes=" << bodyText.size();
    return true;
}

drogon::Task<std::optional<SipServer::PreviewStartResult>> SipServer::startPreviewCoro(const std::string& deviceId, const std::string& channelId) {
    LOG_DEBUG << "[GB28181][Preview] Start requested, device=" << deviceId
             << ", channel=" << channelId;

    const auto route = deviceRegistry_.findRouteSnapshot(deviceId, channelId);
    const auto unavailableReason = routeUnavailableReason(route);
    if (!unavailableReason.empty()) {
        LOG_WARN << "[GB28181][Preview] Start skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=" << unavailableReason;
        co_return std::nullopt;
    }
    if (route->hasChannels && !route->channelExists) {
        LOG_WARN << "[GB28181][Preview] Start skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=channel_not_found";
        co_return std::nullopt;
    }

    const auto endpoint = parseRemoteEndpoint(route->remoteAddress);
    if (!endpoint.has_value()) {
        LOG_WARN << "[GB28181][Preview] Start skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=invalid_remote_address"
                 << ", remote_address=" << route->remoteAddress;
        co_return std::nullopt;
    }

    auto remote = peerFromAddress(route->remoteAddress);
    if (!remote.has_value()) {
        LOG_WARN << "[GB28181][Preview] Start skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=peer_unavailable"
                 << ", remote_address=" << route->remoteAddress;
        co_return std::nullopt;
    }

    std::vector<std::string> staleSessionIds;
    {
        std::lock_guard lock(sessionMutex_);
        for (auto& [_, session] : previewSessions_) {
            if (session.mode == "preview" && session.deviceId == deviceId && session.channelId == channelId) {
                if (!session.mediaOnline) {
                    staleSessionIds.push_back(session.sessionId);
                    continue;
                }
                const auto viewerId = makeToken("viewer");
                ++session.viewerCount;
                previewViewers_[viewerId] = session.sessionId;
                LOG_DEBUG << "[GB28181][Preview] Stream reused, device=" << deviceId
                         << ", channel=" << channelId
                         << ", viewer_session=" << viewerId
                         << ", stream_session=" << session.sessionId
                         << ", stream_id=" << session.streamId
                         << ", viewers=" << session.viewerCount;
                co_return PreviewStartResult{
                    viewerId,
                    session.deviceId,
                    session.channelId,
                    session.streamId,
                    session.ssrc,
                    session.rtpPort,
                    session.playUrls,
                };
            }
        }
    }

    for (const auto& staleSessionId : staleSessionIds) {
        const auto result = co_await stopPreviewCoro(staleSessionId);
        if (result.has_value()) {
            LOG_DEBUG << "[GB28181][Preview] Closed stale session before restart, session=" << staleSessionId
                     << ", stream_id=" << result->streamId
                     << ", bye_sent=" << result->byeSent
                     << ", rtp_server_closed=" << result->rtpServerClosed;
        }
    }

    const auto cseq = cseq_.fetch_add(1);
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto mediaHost = mediaConfig_.rtpPublicIp.empty() || mediaConfig_.rtpPublicIp == "YOUR_PUBLIC_SERVER_IP" ? publicHost : mediaConfig_.rtpPublicIp;
    const auto sessionId = makeToken("preview");
    const auto branch = "z9hG4bK-" + makeToken("branch");
    const auto fromTag = makeToken("tag");
    const auto callId = sessionId + "@" + sipConfig_.domain;
    const auto nowMs = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto ssrcNumber = 1000000000ULL + ((nowMs + cseq) % 899999999ULL);
    const auto ssrc = std::to_string(ssrcNumber);
    LOG_DEBUG << "[GB28181][Preview] Opening ZLM RTP server, device=" << deviceId
             << ", channel=" << channelId
             << ", ssrc=" << ssrc;
    const auto rtpServer = co_await zlmClient_.openRtpServerCoro(deviceId, channelId, ssrc);
    if (!rtpServer.has_value()) {
        LOG_WARN << "[GB28181][Preview] Start failed, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=zlm_open_rtp_failed"
                 << ", ssrc=" << ssrc;
        co_return std::nullopt;
    }
    LOG_DEBUG << "[GB28181][Preview] ZLM RTP server opened, stream_id=" << rtpServer->streamId
             << ", rtp_port=" << rtpServer->port
             << ", ssrc=" << ssrc;

    std::ostringstream body;
    body << "v=0\r\n"
         << "o=" << channelId << " 0 0 IN IP4 " << mediaHost << "\r\n"
         << "s=Play\r\n"
         << "c=IN IP4 " << mediaHost << "\r\n"
         << "t=0 0\r\n"
         << "m=video " << rtpServer->port << " TCP/RTP/AVP 96\r\n"
         << "a=recvonly\r\n"
         << "a=setup:passive\r\n"
         << "a=connection:new\r\n"
         << "a=rtpmap:96 PS/90000\r\n"
         << "y=" << ssrc << "\r\n";

    const auto bodyText = body.str();
    std::ostringstream request;
    request << "INVITE sip:" << channelId << "@" << endpoint->host << ":" << endpoint->port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << fromTag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << cseq << " INVITE\r\n"
            << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
            << "Max-Forwards: 70\r\n"
            << "Subject: " << channelId << ":" << ssrc << "," << sipConfig_.id << ":0\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: application/sdp\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    PreviewSession session;
    session.sessionId = sessionId;
    session.deviceId = deviceId;
    session.channelId = channelId;
    session.streamId = rtpServer->streamId;
    session.callId = callId;
    session.fromTag = fromTag;
    session.branch = branch;
    session.ssrc = ssrc;
    session.inviteCseq = cseq;
    session.rtpPort = rtpServer->port;
    session.playUrls = rtpServer->playUrls;
    session.remote = *remote;
    session.viewerCount = 1;

    const auto viewerId = makeToken("viewer");

    {
        std::lock_guard lock(sessionMutex_);
        previewSessions_.emplace(sessionId, session);
        previewViewers_.emplace(viewerId, sessionId);
    }
    LOG_DEBUG << "[GB28181][Preview] Session stored, viewer_session=" << viewerId
             << ", stream_session=" << sessionId
             << ", stream_id=" << session.streamId
             << ", call_id=" << callId
             << ", cseq=" << cseq;

    sendRequest(request.str(), *remote);
    LOG_DEBUG << "[GB28181][Preview] INVITE sent, device=" << deviceId
             << ", channel=" << channelId
             << ", viewer_session=" << viewerId
             << ", stream_session=" << sessionId
             << ", stream_id=" << rtpServer->streamId
             << ", rtp_port=" << rtpServer->port
             << ", ssrc=" << ssrc
             << ", call_id=" << callId
             << ", branch=" << branch
             << ", remote=" << transportName(remote->transport) << " " << peerToString(*remote)
             << ", sdp=\"" << compactForLog(bodyText, 700) << "\"";

    co_return PreviewStartResult{
        viewerId,
        session.deviceId,
        session.channelId,
        session.streamId,
        session.ssrc,
        session.rtpPort,
        session.playUrls,
    };
}

void SipServer::markStreamOnline(const std::string& streamId, bool online) {
    std::lock_guard lock(sessionMutex_);
    bool found = false;
    for (auto& [_, session] : previewSessions_) {
        if (session.streamId == streamId) {
            found = true;
            session.mediaOnline = online;
            LOG_DEBUG << "[GB28181][Media] Session media state changed, session=" << session.sessionId
                     << ", mode=" << session.mode
                     << ", device=" << session.deviceId
                     << ", channel=" << session.channelId
                     << ", stream_id=" << streamId
                     << ", online=" << online
                     << ", viewers=" << session.viewerCount;
        }
    }
    if (!found) {
        LOG_DEBUG << "[GB28181][Media] Stream state ignored because session was not found, stream_id="
                  << streamId << ", online=" << online;
    }
}

drogon::Task<std::optional<SipServer::PreviewStartResult>> SipServer::startPlaybackCoro(const std::string& deviceId, const std::string& channelId, const std::string& startTime, const std::string& endTime) {
    LOG_DEBUG << "[GB28181][Playback] Start requested, device=" << deviceId
             << ", channel=" << channelId
             << ", start_time=" << startTime
             << ", end_time=" << endTime;

    const auto route = deviceRegistry_.findRouteSnapshot(deviceId);
    const auto unavailableReason = routeUnavailableReason(route);
    if (!unavailableReason.empty()) {
        LOG_WARN << "[GB28181][Playback] Start skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=" << unavailableReason;
        co_return std::nullopt;
    }

    const auto endpoint = parseRemoteEndpoint(route->remoteAddress);
    if (!endpoint.has_value()) {
        LOG_WARN << "[GB28181][Playback] Start skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=invalid_remote_address"
                 << ", remote_address=" << route->remoteAddress;
        co_return std::nullopt;
    }

    auto remote = peerFromAddress(route->remoteAddress);
    if (!remote.has_value()) {
        LOG_WARN << "[GB28181][Playback] Start skipped, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=peer_unavailable"
                 << ", remote_address=" << route->remoteAddress;
        co_return std::nullopt;
    }

    const auto cseq = cseq_.fetch_add(1);
    const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
    const auto mediaHost = mediaConfig_.rtpPublicIp.empty() || mediaConfig_.rtpPublicIp == "YOUR_PUBLIC_SERVER_IP" ? publicHost : mediaConfig_.rtpPublicIp;
    const auto sessionId = makeToken("playback");
    const auto branch = "z9hG4bK-" + makeToken("branch");
    const auto fromTag = makeToken("tag");
    const auto callId = sessionId + "@" + sipConfig_.domain;
    const auto nowMs = static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto ssrcNumber = 2000000000ULL + ((nowMs + cseq) % 899999999ULL);
    const auto ssrc = std::to_string(ssrcNumber);
    LOG_DEBUG << "[GB28181][Playback] Opening ZLM RTP server, device=" << deviceId
             << ", channel=" << channelId
             << ", ssrc=" << ssrc;
    const auto rtpServer = co_await zlmClient_.openRtpServerCoro(deviceId, channelId, ssrc, "playback");
    if (!rtpServer.has_value()) {
        LOG_WARN << "[GB28181][Playback] Start failed, device=" << deviceId
                 << ", channel=" << channelId
                 << ", reason=zlm_open_rtp_failed"
                 << ", ssrc=" << ssrc;
        co_return std::nullopt;
    }
    LOG_DEBUG << "[GB28181][Playback] ZLM RTP server opened, stream_id=" << rtpServer->streamId
             << ", rtp_port=" << rtpServer->port
             << ", ssrc=" << ssrc;

    const auto startSeconds = gbTimeToUnixSeconds(startTime);
    const auto endSeconds = gbTimeToUnixSeconds(endTime);
    if (startSeconds == 0 || endSeconds == 0 || endSeconds <= startSeconds) {
        LOG_WARN << "[GB28181][Playback] Time range converted to an unusual value, device=" << deviceId
                 << ", channel=" << channelId
                 << ", start_time=" << startTime
                 << ", end_time=" << endTime
                 << ", start_seconds=" << startSeconds
                 << ", end_seconds=" << endSeconds;
    }

    std::ostringstream body;
    body << "v=0\r\n"
         << "o=" << channelId << " 0 0 IN IP4 " << mediaHost << "\r\n"
         << "s=Playback\r\n"
         << "u=" << channelId << ":0\r\n"
         << "c=IN IP4 " << mediaHost << "\r\n"
         << "t=" << startSeconds << " " << endSeconds << "\r\n"
         << "m=video " << rtpServer->port << " TCP/RTP/AVP 96\r\n"
         << "a=recvonly\r\n"
         << "a=setup:passive\r\n"
         << "a=connection:new\r\n"
         << "a=rtpmap:96 PS/90000\r\n"
         << "y=" << ssrc << "\r\n";

    const auto bodyText = body.str();
    std::ostringstream request;
    request << "INVITE sip:" << channelId << "@" << endpoint->host << ":" << endpoint->port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(remote->transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << fromTag << "\r\n"
            << "To: <sip:" << channelId << "@" << sipConfig_.domain << ">\r\n"
            << "Call-ID: " << callId << "\r\n"
            << "CSeq: " << cseq << " INVITE\r\n"
            << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
            << "Max-Forwards: 70\r\n"
            << "Subject: " << channelId << ":" << ssrc << "," << sipConfig_.id << ":0\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Type: application/sdp\r\n"
            << "Content-Length: " << bodyText.size() << "\r\n\r\n"
            << bodyText;

    PreviewSession session;
    session.sessionId = sessionId;
    session.deviceId = deviceId;
    session.channelId = channelId;
    session.streamId = rtpServer->streamId;
    session.callId = callId;
    session.fromTag = fromTag;
    session.branch = branch;
    session.ssrc = ssrc;
    session.inviteCseq = cseq;
    session.rtpPort = rtpServer->port;
    session.playUrls = rtpServer->playUrls;
    session.remote = *remote;
    session.mode = "playback";

    {
        std::lock_guard lock(sessionMutex_);
        previewSessions_[sessionId] = session;
    }

    sendRequest(request.str(), *remote);
    LOG_DEBUG << "[GB28181][Playback] INVITE sent, device=" << deviceId
             << ", channel=" << channelId
             << ", session=" << sessionId
             << ", stream_id=" << rtpServer->streamId
             << ", rtp_port=" << rtpServer->port
             << ", ssrc=" << ssrc
             << ", call_id=" << callId
             << ", branch=" << branch
             << ", remote=" << transportName(remote->transport) << " " << peerToString(*remote)
             << ", sdp=\"" << compactForLog(bodyText, 700) << "\"";

    co_return PreviewStartResult{
        session.sessionId,
        session.deviceId,
        session.channelId,
        session.streamId,
        session.ssrc,
        session.rtpPort,
        session.playUrls,
    };
}
drogon::Task<std::optional<SipServer::PreviewStopResult>> SipServer::stopPreviewCoro(const std::string& sessionId) {
    LOG_DEBUG << "[GB28181][Preview] Stop requested, session=" << sessionId;

    PreviewSession session;
    std::string streamSessionId = sessionId;
    {
        std::lock_guard lock(sessionMutex_);
        const auto viewerIter = previewViewers_.find(sessionId);
        if (viewerIter != previewViewers_.end()) {
            streamSessionId = viewerIter->second;
            previewViewers_.erase(viewerIter);
        }

        const auto iter = previewSessions_.find(streamSessionId);
        if (iter == previewSessions_.end()) {
            LOG_WARN << "[GB28181][Preview] Stop skipped, session=" << sessionId
                     << ", resolved_stream_session=" << streamSessionId
                     << ", reason=session_not_found";
            co_return std::nullopt;
        }

        if (iter->second.mode == "preview" && iter->second.viewerCount > 1 && streamSessionId != sessionId) {
            --iter->second.viewerCount;
            LOG_DEBUG << "[GB28181][Preview] Viewer released, viewer_session=" << sessionId
                     << ", stream_session=" << streamSessionId
                     << ", stream_id=" << iter->second.streamId
                     << ", viewers=" << iter->second.viewerCount;
            co_return PreviewStopResult{
                sessionId,
                iter->second.streamId,
                false,
                false,
            };
        }

        session = iter->second;
        for (auto viewer = previewViewers_.begin(); viewer != previewViewers_.end();) {
            if (viewer->second == streamSessionId) {
                viewer = previewViewers_.erase(viewer);
            } else {
                ++viewer;
            }
        }
        previewSessions_.erase(iter);
    }

    LOG_DEBUG << "[GB28181][Preview] Stream session removed, requested_session=" << sessionId
             << ", stream_session=" << streamSessionId
             << ", mode=" << session.mode
             << ", device=" << session.deviceId
             << ", channel=" << session.channelId
             << ", stream_id=" << session.streamId
             << ", established=" << session.established
             << ", media_online=" << session.mediaOnline
             << ", viewers=" << session.viewerCount;

    bool byeSent = false;
    if (session.established) {
        const auto cseq = cseq_.fetch_add(1);
        const auto publicHost = sipConfig_.publicIp.empty() || sipConfig_.publicIp == "YOUR_PUBLIC_SERVER_IP" ? sipConfig_.host : sipConfig_.publicIp;
        const auto branch = "z9hG4bK-" + makeToken("bye");
        const auto host = session.remote.address;
        const auto port = session.remote.port;

        std::ostringstream bye;
        bye << "BYE sip:" << session.channelId << "@" << host << ":" << port << " SIP/2.0\r\n"
            << "Via: SIP/2.0/" << transportName(session.remote.transport) << " " << publicHost << ":" << sipConfig_.port << ";branch=" << branch << "\r\n"
            << "From: <sip:" << sipConfig_.id << "@" << sipConfig_.domain << ">;tag=" << session.fromTag << "\r\n"
            << "To: <sip:" << session.channelId << "@" << sipConfig_.domain << ">;tag=" << session.toTag << "\r\n"
            << "Call-ID: " << session.callId << "\r\n"
            << "CSeq: " << cseq << " BYE\r\n"
            << "Contact: <sip:" << sipConfig_.id << "@" << publicHost << ":" << sipConfig_.port << ">\r\n"
            << "Max-Forwards: 70\r\n"
            << "User-Agent: gb28181-platform-cpp\r\n"
            << "Content-Length: 0\r\n\r\n";

        sendRequest(bye.str(), session.remote);
        byeSent = true;
        LOG_DEBUG << "[GB28181][Preview] BYE sent, session=" << streamSessionId
                 << ", mode=" << session.mode
                 << ", stream_id=" << session.streamId
                 << ", call_id=" << session.callId
                 << ", cseq=" << cseq
                 << ", remote=" << transportName(session.remote.transport) << " " << peerToString(session.remote);
    } else {
        LOG_DEBUG << "[GB28181][Preview] BYE skipped because session was not established, session="
                 << streamSessionId
                 << ", stream_id=" << session.streamId
                 << ", call_id=" << session.callId;
    }

    const bool rtpServerClosed = co_await zlmClient_.closeRtpServerCoro(session.streamId);
    LOG_DEBUG << "[GB28181][Preview] RTP server close finished, session=" << streamSessionId
             << ", stream_id=" << session.streamId
             << ", closed=" << rtpServerClosed
             << ", bye_sent=" << byeSent;

    co_return PreviewStopResult{
        sessionId,
        session.streamId,
        byeSent,
        rtpServerClosed,
    };
}

drogon::Task<std::optional<SipServer::PreviewStopResult>> SipServer::stopPreviewByStreamCoro(const std::string& streamId) {
    LOG_DEBUG << "[GB28181][Preview] Stop by stream requested, stream_id=" << streamId;

    std::string sessionId;
    {
        std::lock_guard lock(sessionMutex_);
        for (const auto& [candidateSessionId, session] : previewSessions_) {
            if (session.streamId == streamId) {
                sessionId = candidateSessionId;
                break;
            }
        }
    }

    if (sessionId.empty()) {
        LOG_WARN << "[GB28181][Preview] Stop by stream skipped, stream_id=" << streamId
                 << ", reason=session_not_found";
        co_return std::nullopt;
    }
    co_return co_await stopPreviewCoro(sessionId);
}

drogon::Task<bool> SipServer::forceCloseRtpServerCoro(const std::string& streamId) {
    if (streamId.empty()) {
        LOG_WARN << "[GB28181][Media] Force close RTP skipped, reason=stream_id_empty";
        co_return false;
    }
    LOG_WARN << "[GB28181][Media] Force closing orphan RTP server, stream_id=" << streamId;
    const auto closed = co_await zlmClient_.closeRtpServerCoro(streamId);
    LOG_WARN << "[GB28181][Media] Force close RTP finished, stream_id=" << streamId
             << ", closed=" << closed;
    co_return closed;
}

void SipServer::sendResponse(const SipMessage& request, const SipPeer& remote, int statusCode, const std::string& reason, const std::string& extraHeaders) {
    std::ostringstream response;
    response << "SIP/2.0 " << statusCode << ' ' << reason << "\r\n";

    const auto via = request.header("Via");
    const auto from = request.header("From");
    const auto to = request.header("To");
    const auto callId = request.header("Call-ID");
    const auto cseq = request.header("CSeq");

    if (!via.empty()) {
        response << "Via: " << via << "\r\n";
    }
    if (!from.empty()) {
        response << "From: " << from << "\r\n";
    }
    if (!to.empty()) {
        response << "To: " << to << "\r\n";
    }
    if (!callId.empty()) {
        response << "Call-ID: " << callId << "\r\n";
    }
    if (!cseq.empty()) {
        response << "CSeq: " << cseq << "\r\n";
    }

    response << "User-Agent: gb28181-platform-cpp\r\n";
    response << extraHeaders;
    response << "Content-Length: 0\r\n\r\n";

    const auto data = response.str();
    sendRequest(data, remote);
    if (sipConfig_.logging) {
        LOG_DEBUG << "[GB28181][SIP] Response sent, status=" << statusCode
                  << ", reason=\"" << reason << "\""
                  << ", remote=" << transportName(remote.transport) << " " << peerToString(remote)
                  << ", call_id=" << callId
                  << ", cseq=\"" << cseq << "\"";
    }
}

void SipServer::sendRequest(const std::string& request, const SipPeer& remote) {
    if (sipConfig_.logging) {
        logSipSend(request, remote, true);
    }

    std::lock_guard lock(sendMutex_);
    if (remote.transport == SipTransport::Tcp) {
        if (!remote.tcp || !remote.tcp->connected()) {
            LOG_WARN << "[GB28181][SIP] TCP send failed: connection is closed, remote="
                     << peerToString(remote);
            return;
        }
        remote.tcp->send(request);
        if (sipConfig_.logging) {
            LOG_DEBUG << "[GB28181][SIP][TCP_TX] remote=" << peerToString(remote)
                      << ", bytes=" << request.size();
        }
        return;
    }

    if (ioLoop_ == nullptr || isInvalidSocket(udpSocket_)) {
        LOG_WARN << "[GB28181][SIP] UDP send failed: socket is not ready, remote="
                 << peerToString(remote);
        return;
    }

    ioLoop_->queueInLoop([this, request, remoteAddress = remote.udp]() {
        if (!running_ || isInvalidSocket(udpSocket_)) {
            return;
        }
        const auto sent = ::sendto(
            udpSocket_,
            request.data(),
            static_cast<int>(request.size()),
            0,
            reinterpret_cast<const sockaddr*>(&remoteAddress),
            sizeof(remoteAddress));
        if (sent < 0) {
            LOG_WARN << "[GB28181][SIP] UDP send failed: " << socketErrorMessage();
        } else if (sipConfig_.logging) {
            LOG_DEBUG << "[GB28181][SIP][UDP_TX] bytes=" << sent;
        }
    });
}
std::optional<SipServer::SipPeer> SipServer::peerFromAddress(const std::string& remoteAddress) const {
    const auto endpoint = parseRemoteEndpoint(remoteAddress);
    if (!endpoint.has_value()) {
        return std::nullopt;
    }

    SipPeer peer;
    peer.address = endpoint->host;
    peer.port = endpoint->port;
    {
        std::lock_guard lock(tcpConnectionsMutex_);
        const auto iter = tcpConnections_.find(remoteAddress);
        if (iter != tcpConnections_.end() && iter->second && iter->second->connected()) {
            peer.transport = SipTransport::Tcp;
            peer.tcp = iter->second;
            return peer;
        }
    }

    peer.transport = SipTransport::Udp;
    peer.udp = makeSocketAddress(peer.address, peer.port);
    return peer;
}

void SipServer::scheduleCatalogQuery(const std::string& deviceId) {
    if (ioLoop_ == nullptr) {
        LOG_WARN << "[GB28181][Catalog] Schedule skipped, device=" << deviceId
                 << ", reason=io_loop_unavailable";
        return;
    }
    LOG_DEBUG << "[GB28181][Catalog] Query scheduled, device=" << deviceId
             << ", delay_seconds=0.5";
    ioLoop_->runAfter(0.5, [this, deviceId]() {
        if (!running_) {
            LOG_DEBUG << "[GB28181][Catalog] Scheduled query skipped, device=" << deviceId
                     << ", reason=server_stopped";
            return;
        }
        if (queryCatalog(deviceId)) {
            LOG_DEBUG << "[GB28181][Catalog] Scheduled query sent, device=" << deviceId;
        } else {
            LOG_WARN << "[GB28181][Catalog] Scheduled query failed, device=" << deviceId;
        }
    });
}
