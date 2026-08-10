#pragma once

#include "config/AppConfig.h"
#include "device/DeviceRegistry.h"
#include "media/ZlmSdk.h"
#include "sip/SipMessage.h"

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <ruvia/core/EventLoopPool.h>

#include <array>
#include <atomic>
#include <deque>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

#ifdef GetMessage
#undef GetMessage
#endif

class SipServer {
public:
    struct PreviewStartResult {
        std::string sessionId;
        std::string deviceId;
        std::string channelId;
        std::string streamId;
        std::string ssrc;
        uint16_t rtpPort{0};
        PlayUrls playUrls;
    };

    struct PreviewStopResult {
        std::string sessionId;
        std::string streamId;
        bool byeSent{false};
        bool rtpServerClosed{false};
    };

    SipServer(SipConfig sipConfig, MediaConfig mediaConfig, DeviceRegistry& deviceRegistry,
              ZlmSdk& zlmSdk, ruvia::EventLoop ioLoop);
    ~SipServer();

    void start();
    void stop();
    bool queryCatalog(const std::string& deviceId);
    bool queryRecords(const std::string& deviceId, const std::string& channelId, const std::string& startTime, const std::string& endTime);
    bool sendPtzControl(const std::string& deviceId, const std::string& channelId, const std::string& action, uint8_t speed);
    bool sendPtzPreciseControl(const std::string& deviceId, const std::string& channelId, double pan, double tilt, double zoom);
    std::optional<PreviewStartResult> startPreview(const std::string& deviceId,
                                                   const std::string& channelId);
    std::optional<PreviewStartResult>
    startPlayback(const std::string& deviceId, const std::string& channelId,
                  const std::string& startTime, const std::string& endTime);
    std::optional<PreviewStopResult> stopPreview(const std::string& sessionId);
    std::optional<PreviewStopResult> stopPreviewByStream(const std::string& streamId);
    void markStreamOnline(const std::string& streamId, bool online);

    enum class SipTransport {
        Udp,
        Tcp,
    };

    struct TcpConnection final {
        explicit TcpConnection(asio::ip::tcp::socket value)
            : socket(std::move(value)) {}

        asio::ip::tcp::socket socket;
        std::string key;
        std::string address;
        std::uint16_t port{0};
        std::array<char, 8192> readBuffer{};
        std::string pending;
        std::deque<std::shared_ptr<std::string>> writes;
        bool writeInProgress{false};
    };

    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

    struct SipPeer {
        SipTransport transport{SipTransport::Udp};
        asio::ip::udp::endpoint udp;
        TcpConnectionPtr tcp;
        std::string address;
        uint16_t port{0};
    };

private:
    enum class DeadlineKind {
        Catalog,
        Registration,
        Invite,
        RecordQuery,
        AuthNonce,
    };

    struct Deadline {
        std::chrono::steady_clock::time_point at;
        DeadlineKind kind{DeadlineKind::Catalog};
        std::string key;
        std::uint64_t generation{0};
    };

    struct DeadlineLater {
        bool operator()(const Deadline& left, const Deadline& right) const noexcept {
            return left.at > right.at;
        }
    };

    struct PreviewSession {
        std::string sessionId;
        std::string deviceId;
        std::string channelId;
        std::string streamId;
        std::string callId;
        std::string fromTag;
        std::string toTag;
        std::string branch;
        std::string ssrc;
        unsigned int inviteCseq{0};
        uint16_t rtpPort{0};
        PlayUrls playUrls;
        SipPeer remote;
        std::string mode{"preview"};
        bool established{false};
        bool mediaOnline{false};
        unsigned int viewerCount{0};
    };

    struct PendingRecordQuery {
        std::string deviceId;
        SipPeer remote;
    };

    SipConfig sipConfig_;
    MediaConfig mediaConfig_;
    DeviceRegistry& deviceRegistry_;
    ZlmSdk& zlmSdk_;
    std::atomic_bool running_{false};
    ruvia::EventLoop ioLoop_;
    std::unique_ptr<asio::ip::udp::socket> udpSocket_;
    std::unique_ptr<asio::ip::tcp::acceptor> tcpAcceptor_;
    std::unique_ptr<asio::steady_timer> deadlineTimer_;
    std::array<char, 8192> udpBuffer_{};
    asio::ip::udp::endpoint udpRemote_;
    std::atomic_uint cseq_{1};
    std::unordered_map<std::string, TcpConnectionPtr> tcpConnections_;
    std::map<std::string, PreviewSession> previewSessions_;
    std::map<std::string, std::string> previewViewers_;
    std::map<unsigned int, PendingRecordQuery> pendingRecordQueries_;
    std::unordered_map<std::string, std::uint32_t> digestNonceCounts_;
    std::unordered_map<std::string, std::uint64_t> deadlineGenerations_;
    std::priority_queue<Deadline, std::vector<Deadline>, DeadlineLater> deadlines_;

    void startInLoop();
    void stopInLoop();
    void receiveUdp();
    void acceptTcp();
    void readTcp(const TcpConnectionPtr& connection);
    void removeTcpConnection(const TcpConnectionPtr& connection);
    void processTcpPending(const TcpConnectionPtr& connection);
    void writeTcp(const TcpConnectionPtr& connection, std::shared_ptr<std::string> data);
    void continueTcpWrite(const TcpConnectionPtr& connection);
    void handlePacket(const std::string& packet, const SipPeer& remote);
    void handleRegister(const SipMessage& message, const SipPeer& remote);
    void handleMessage(const SipMessage& message, const SipPeer& remote);
    void handleResponse(const SipMessage& message, const SipPeer& remote);
    void handleInviteOk(const SipMessage& message, const SipPeer& remote);
    void sendResponse(const SipMessage& request, const SipPeer& remote, int statusCode, const std::string& reason, const std::string& extraHeaders = {});
    void sendRequest(const std::string& request, const SipPeer& remote);
    std::optional<SipPeer> peerFromAddress(const std::string& remoteAddress) const;
    void scheduleCatalogQuery(const std::string& deviceId);
    void scheduleDeadline(DeadlineKind kind, std::string key,
                          std::chrono::milliseconds delay);
    void cancelDeadline(DeadlineKind kind, const std::string& key);
    void armDeadlineTimer();
    void processDeadlines(const std::error_code& error);
    void expireDeadline(const Deadline& deadline);
    [[nodiscard]] static std::string deadlineKey(DeadlineKind kind,
                                                 const std::string& key);
    void closeDeviceSessions(const std::string& deviceId);
    [[nodiscard]] std::optional<std::string>
    sessionIdByCallId(const std::string& callId) const;
};
