#pragma once

#include "config/AppConfig.h"
#include "device/DeviceRegistry.h"
#include "media/ZlmClient.h"
#include "sip/SipMessage.h"

#include <atomic>
#include <drogon/drogon.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <trantor/net/Channel.h>
#include <trantor/net/EventLoop.h>
#include <trantor/net/TcpConnection.h>
#include <trantor/net/TcpServer.h>
#include <trantor/utils/MsgBuffer.h>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

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
              ZlmClient& zlmClient, trantor::EventLoop* ioLoop);
    ~SipServer();

    void start();
    void stop();
    drogon::Task<> startCoro();
    drogon::Task<> stopCoro();
    bool queryCatalog(const std::string& deviceId);
    bool queryRecords(const std::string& deviceId, const std::string& channelId, const std::string& startTime, const std::string& endTime);
    bool sendPtzControl(const std::string& deviceId, const std::string& channelId, const std::string& action, uint8_t speed);
    bool sendPtzPreciseControl(const std::string& deviceId, const std::string& channelId, double pan, double tilt, double zoom);
    drogon::Task<std::optional<PreviewStartResult>> startPreviewCoro(const std::string& deviceId, const std::string& channelId);
    drogon::Task<std::optional<PreviewStartResult>> startPlaybackCoro(const std::string& deviceId, const std::string& channelId, const std::string& startTime, const std::string& endTime);
    drogon::Task<std::optional<PreviewStopResult>> stopPreviewCoro(const std::string& sessionId);
    drogon::Task<std::optional<PreviewStopResult>> stopPreviewByStreamCoro(const std::string& streamId);
    drogon::Task<bool> forceCloseRtpServerCoro(const std::string& streamId);
    void markStreamOnline(const std::string& streamId, bool online);

    enum class SipTransport {
        Udp,
        Tcp,
    };

    using TcpConnectionPtr = trantor::TcpConnectionPtr;

    struct SipPeer {
        SipTransport transport{SipTransport::Udp};
        sockaddr_in udp{};
        TcpConnectionPtr tcp;
        std::string address;
        uint16_t port{0};
    };

private:
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

    SipConfig sipConfig_;
    MediaConfig mediaConfig_;
    DeviceRegistry& deviceRegistry_;
    ZlmClient& zlmClient_;
    std::atomic_bool running_{false};
    trantor::EventLoop* ioLoop_{nullptr};
    std::unique_ptr<trantor::Channel> udpChannel_;
    std::shared_ptr<trantor::TcpServer> tcpServer_;
#ifdef _WIN32
    SOCKET udpSocket_{INVALID_SOCKET};
#else
    int udpSocket_{-1};
#endif
    std::mutex sendMutex_;
    mutable std::mutex tcpConnectionsMutex_;
    std::mutex sessionMutex_;
    std::atomic_uint cseq_{1};
    std::unordered_map<std::string, TcpConnectionPtr> tcpConnections_;
    std::map<std::string, PreviewSession> previewSessions_;
    std::map<std::string, std::string> previewViewers_;
    std::map<unsigned int, std::string> pendingRecordQueries_;

    void startInLoop();
    void stopInLoop();
    void handleUdpReadable();
    void handleTcpConnection(const TcpConnectionPtr& connection);
    void handleTcpMessage(const TcpConnectionPtr& connection, trantor::MsgBuffer* buffer);
    void handlePacket(const std::string& packet, const SipPeer& remote);
    void handleRegister(const SipMessage& message, const SipPeer& remote);
    void handleMessage(const SipMessage& message, const SipPeer& remote);
    void handleResponse(const SipMessage& message, const SipPeer& remote);
    void handleInviteOk(const SipMessage& message, const SipPeer& remote);
    void sendResponse(const SipMessage& request, const SipPeer& remote, int statusCode, const std::string& reason, const std::string& extraHeaders = {});
    void sendRequest(const std::string& request, const SipPeer& remote);
    std::optional<SipPeer> peerFromAddress(const std::string& remoteAddress) const;
    void scheduleCatalogQuery(const std::string& deviceId);
};
