#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>

#if defined(SO_REUSEPORT)
#include <sys/socket.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "service/common/message/contract.h"
#include "service/features/collector/engine.h"
#include "service/features/collector/types.h"
#include "service/features/collector/timer.h"

namespace service::collector {

struct LinkState {
    struct Target {
        std::string id;
        std::string state;
        std::string reason;
        std::string error;
        std::int64_t lastActivityAtMs = 0;
    };

    std::string linkId;
    std::size_t workerIndex = 0;
    std::string state;
    std::string reason;
    std::string error;
    std::vector<std::string> remoteEndpoints;
    std::vector<Target> targets;
    std::int64_t lastActivityAtMs = 0;
};

class Tcp final {
  public:
    using NativeSocket = asio::ip::tcp::socket::native_handle_type;
    using ConnectedHandler = std::function<void(ProtocolConnectionInfo)>;
    using PacketHandler = std::function<void(message::IngressPacket)>;
    using DisconnectedHandler = std::function<void(std::string, std::string)>;
    using StateHandler = std::function<void(LinkState)>;
    using ServerAcceptHandler =
        std::function<void(std::string, NativeSocket, std::string)>;
    using WriteHandler = std::function<void(bool)>;

    Tcp(asio::io_context& ioContext, Timer& scheduler,
                     std::size_t workerIndex, std::size_t workerCount,
                     ConnectedHandler onConnected, PacketHandler onPacket,
                     DisconnectedHandler onDisconnected, StateHandler onState,
                     bool listenServers, ServerAcceptHandler onServerAccept)
        : ioContext_(ioContext), scheduler_(scheduler), workerIndex_(workerIndex),
          workerCount_(workerCount), onConnected_(std::move(onConnected)),
          onPacket_(std::move(onPacket)), onDisconnected_(std::move(onDisconnected)),
          onState_(std::move(onState)), listenServers_(listenServers),
          onServerAccept_(std::move(onServerAccept)) {}

    Tcp(const Tcp&) = delete;
    Tcp& operator=(const Tcp&) = delete;

    ~Tcp() { stop(); }

    void reload(const RuntimeSnapshot& snapshot) {
        stopTransports();
        stopped_ = false;
        definitions_.clear();
        for (const auto& link : snapshot.links) {
            definitions_.emplace(link.id, link);
            startDefinition(link);
        }
    }

    void stopLinks(const std::set<std::string, std::less<>>& linkIds) {
        for (auto current = listeners_.begin(); current != listeners_.end();) {
            if (!linkIds.contains(current->first)) {
                ++current;
                continue;
            }
            current->second->stop();
            current = listeners_.erase(current);
        }
        for (auto current = clients_.begin(); current != clients_.end();) {
            if (!linkIds.contains(current->second->link.id)) {
                ++current;
                continue;
            }
            current->second->stop();
            current = clients_.erase(current);
        }
        std::vector<std::string> closing;
        for (const auto& [connectionId, linkId] : connectionLinks_)
            if (linkIds.contains(linkId))
                closing.push_back(connectionId);
        for (const auto& connectionId : closing) {
            const auto current = connections_.find(connectionId);
            if (current != connections_.end())
                current->second->close("config_reloaded");
        }
        for (const auto& linkId : linkIds)
            lastActivity_.erase(linkId);
    }

    void reconcile(const RuntimeSnapshot& snapshot,
                   const std::set<std::string, std::less<>>& affectedLinks) {
        stopped_ = false;
        for (const auto& linkId : affectedLinks)
            definitions_.erase(linkId);
        for (const auto& link : snapshot.links) {
            if (!affectedLinks.contains(link.id))
                continue;
            definitions_.insert_or_assign(link.id, link);
            startDefinition(link);
        }
    }

    void stop() noexcept {
        if (stopped_)
            return;
        stopped_ = true;
        stopTransports();
        definitions_.clear();
    }

    void close(std::string_view connectionId, std::string reason = "local_closed") {
        const auto current = connections_.find(connectionId);
        if (current != connections_.end())
            current->second->close(std::move(reason));
    }

    void send(std::string_view connectionId, std::vector<std::uint8_t> bytes,
              WriteHandler onDone = {}) {
        const auto current = connections_.find(connectionId);
        if (current == connections_.end()) {
            if (onDone)
                onDone(false);
            return;
        }
        current->second->send(std::move(bytes), std::move(onDone));
    }

    [[nodiscard]] std::vector<std::string> connectionIds(std::string_view linkId) const {
        std::vector<std::string> result;
        for (const auto& [connectionId, ownerLinkId] : connectionLinks_)
            if (ownerLinkId == linkId)
                result.push_back(connectionId);
        return result;
    }

    void adoptServerSocket(std::string linkId, NativeSocket handle, std::string remote) {
        const auto definition = definitions_.find(linkId);
        if (stopped_ || definition == definitions_.end() ||
            definition->second.mode != "TCP Server" || definition->second.status != "enabled") {
            closeNative(handle);
            return;
        }
        asio::ip::tcp::socket socket(ioContext_);
        std::error_code error;
        socket.assign(asio::ip::tcp::v4(), handle, error);
        if (error) {
            closeNative(handle);
            publish(linkId, "error", "socket_handoff_failed", error.message());
            return;
        }
        addConnection(definition->second, {}, message::nextMessageId(), std::move(remote),
                      std::move(socket));
    }

    static void closeNative(NativeSocket handle) noexcept {
        if (handle < 0)
            return;
#if defined(_WIN32)
        ::closesocket(handle);
#else
        ::close(handle);
#endif
    }

  private:
    struct Failure {
        std::string_view reason;
        std::string_view message;
        bool retry = true;
    };

    static Failure classify(const std::error_code& error) {
        if (error == asio::error::eof)
            return {"remote_closed", "远端已关闭连接", true};
        if (error == asio::error::connection_refused)
            return {"connection_refused", "目标拒绝连接", true};
        if (error == asio::error::connection_reset)
            return {"connection_reset", "连接被远端重置", true};
        if (error == asio::error::timed_out)
            return {"timed_out", "连接超时", true};
        if (error == asio::error::host_unreachable)
            return {"host_unreachable", "目标主机不可达", true};
        if (error == asio::error::network_unreachable)
            return {"network_unreachable", "目标网络不可达", true};
        if (error == asio::error::invalid_argument)
            return {"invalid_address", "目标地址无效", false};
        if (error == asio::error::operation_aborted)
            return {"local_closed", "本地已关闭连接", false};
        return {"transport_error", "链路传输异常", true};
    }

    class Connection final : public std::enable_shared_from_this<Connection> {
      public:
        using BytesHandler =
            std::function<void(std::string_view, std::string_view, std::vector<std::uint8_t>)>;
        using CloseHandler = std::function<void(std::string_view, std::string_view)>;

        Connection(asio::ip::tcp::socket socket, std::string id, std::string remote,
                   BytesHandler onBytes, CloseHandler onClose)
            : socket_(std::move(socket)), id_(std::move(id)), remote_(std::move(remote)),
              onBytes_(std::move(onBytes)), onClose_(std::move(onClose)) {}

        [[nodiscard]] const std::string& id() const noexcept { return id_; }
        [[nodiscard]] const std::string& remote() const noexcept { return remote_; }

        void start() { read(); }

        void close(std::string reason) { finish(std::move(reason)); }

        void send(std::vector<std::uint8_t> bytes, WriteHandler handler) {
            if (closed_) {
                if (handler)
                    handler(false);
                return;
            }
            writes_.push_back({std::move(bytes), std::move(handler)});
            if (!writing_)
                writeNext();
        }

      private:
        struct PendingWrite {
            std::vector<std::uint8_t> bytes;
            WriteHandler handler;
        };

        void read() {
            socket_.async_read_some(asio::buffer(readBuffer_), [self = shared_from_this()](
                                                                   const std::error_code& error,
                                                                   std::size_t size) {
                if (error) {
                    self->finish(std::string(classify(error).reason));
                    return;
                }
                std::vector<std::uint8_t> bytes(
                    self->readBuffer_.begin(),
                    self->readBuffer_.begin() + static_cast<std::ptrdiff_t>(size));
                self->onBytes_(self->id_, self->remote_, std::move(bytes));
                self->read();
            });
        }

        void writeNext() {
            if (closed_ || writes_.empty()) {
                writing_ = false;
                return;
            }
            writing_ = true;
            asio::async_write(socket_, asio::buffer(writes_.front().bytes),
                              [self = shared_from_this()](const std::error_code& error,
                                                          std::size_t) {
                auto handler = std::move(self->writes_.front().handler);
                self->writes_.pop_front();
                if (handler)
                    handler(!error);
                if (error) {
                    self->finish(std::string(classify(error).reason));
                    return;
                }
                self->writeNext();
            });
        }

        void finish(std::string reason) {
            if (closed_)
                return;
            closed_ = true;
            std::error_code ignored;
            socket_.cancel(ignored);
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
            socket_.close(ignored);
            for (auto& pending : writes_)
                if (pending.handler)
                    pending.handler(false);
            writes_.clear();
            onClose_(id_, reason);
        }

        asio::ip::tcp::socket socket_;
        std::string id_;
        std::string remote_;
        BytesHandler onBytes_;
        CloseHandler onClose_;
        std::array<std::uint8_t, 16384> readBuffer_{};
        std::deque<PendingWrite> writes_;
        bool writing_ = false;
        bool closed_ = false;
    };

    class Listener final : public std::enable_shared_from_this<Listener> {
      public:
        using AcceptHandler = std::function<void(asio::ip::tcp::socket, std::string)>;

        Listener(asio::io_context& ioContext, const LinkDefinition& link, AcceptHandler onAccept)
            : acceptor_(ioContext), onAccept_(std::move(onAccept)) {
            const auto address = asio::ip::make_address("0.0.0.0");
            const asio::ip::tcp::endpoint endpoint(address, link.port);
            acceptor_.open(endpoint.protocol());
            acceptor_.set_option(asio::socket_base::reuse_address(true));
#if defined(SO_REUSEPORT)
            const int enabled = 1;
            if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled,
                             sizeof(enabled)) != 0)
                throw std::system_error(errno, std::generic_category(), "SO_REUSEPORT");
#endif
            acceptor_.bind(endpoint);
            acceptor_.listen(asio::socket_base::max_listen_connections);
        }

        void start() { accept(); }

        void stop() noexcept {
            std::error_code ignored;
            acceptor_.cancel(ignored);
            acceptor_.close(ignored);
        }

      private:
        void accept() {
            acceptor_.async_accept([self = shared_from_this()](const std::error_code& error,
                                                               asio::ip::tcp::socket socket) {
                if (!error) {
                    std::string remote;
                    std::error_code endpointError;
                    const auto endpoint = socket.remote_endpoint(endpointError);
                    if (!endpointError)
                        remote = endpoint.address().to_string() + ':' +
                                 std::to_string(endpoint.port());
                    self->onAccept_(std::move(socket), std::move(remote));
                }
                if (self->acceptor_.is_open())
                    self->accept();
            });
        }

        asio::ip::tcp::acceptor acceptor_;
        AcceptHandler onAccept_;
    };

    struct ClientTarget : std::enable_shared_from_this<ClientTarget> {
        Tcp& owner;
        LinkDefinition link;
        LinkTargetDefinition target;
        asio::ip::tcp::socket socket;
        Timer::Token reconnectToken = 0;
        std::string connectionId;
        bool stopped = false;
        bool failed = false;
        std::string state = "connecting";
        std::string reason;
        std::string error;
        std::int64_t lastActivityAtMs = 0;

        ClientTarget(Tcp& ownerValue, LinkDefinition linkValue,
                     LinkTargetDefinition targetValue)
            : owner(ownerValue), link(std::move(linkValue)), target(std::move(targetValue)),
              socket(owner.ioContext_) {}

        [[nodiscard]] std::string key() const { return link.id + ':' + target.id; }
        [[nodiscard]] std::string endpoint() const {
            return target.ip + ':' + std::to_string(target.port);
        }

        void start() { connect(); }

        void stop() {
            stopped = true;
            if (reconnectToken != 0)
                owner.scheduler_.cancel(reconnectToken);
            std::error_code ignored;
            socket.cancel(ignored);
            socket.close(ignored);
            if (!connectionId.empty())
                owner.removeConnection(connectionId, "local_closed", false);
            connectionId.clear();
        }

        void connect() {
            if (stopped)
                return;
            state = failed ? "reconnecting" : "connecting";
            reason.clear();
            error.clear();
            owner.publish(link.id, failed ? "reconnecting" : "connecting");
            std::error_code ignored;
            socket.close(ignored);
            socket = asio::ip::tcp::socket(owner.ioContext_);
            std::error_code addressError;
            const auto address = asio::ip::make_address(target.ip, addressError);
            if (addressError) {
                fail(addressError);
                return;
            }
            socket.async_connect({address, target.port}, [self = shared_from_this()](
                                                               const std::error_code& error) {
                if (error) {
                    self->fail(error);
                    return;
                }
                self->connectionId = message::nextMessageId();
                self->state = "connected";
                self->reason.clear();
                self->error.clear();
                self->owner.addConnection(self->link, self->target.id, self->connectionId,
                                          self->endpoint(), std::move(self->socket));
                self->owner.publish(self->link.id, "connected");
            });
        }

        void fail(const std::error_code& error) {
            if (stopped)
                return;
            std::error_code ignored;
            socket.close(ignored);
            if (!connectionId.empty())
                owner.removeConnection(connectionId, std::string(classify(error).reason), false);
            connectionId.clear();
            const auto failure = classify(error);
            failed = true;
            state = failure.retry ? "reconnecting" : "error";
            reason = failure.reason;
            this->error = failure.message;
            owner.publish(link.id, failure.retry ? "reconnecting" : "error", failure.reason,
                          failure.message);
            if (!failure.retry)
                return;
            reconnectToken = owner.scheduler_.scheduleAfter(std::chrono::seconds(2),
                                                             [self = shared_from_this()] {
                self->reconnectToken = 0;
                self->connect();
            });
        }
    };

    void startServer(const LinkDefinition& link) {
        try {
            auto listener = std::make_shared<Listener>(
                ioContext_, link, [this, link](asio::ip::tcp::socket socket, std::string remote) {
                    std::error_code error;
                    const auto handle = socket.release(error);
                    if (error) {
                        publish(link.id, "error", "socket_handoff_failed", error.message());
                        return;
                    }
                    if (onServerAccept_)
                        onServerAccept_(link.id, handle, std::move(remote));
                    else
                        closeNative(handle);
                });
            listeners_.emplace(link.id, listener);
            listener->start();
            publish(link.id, "listening");
        } catch (const std::exception& error) {
            publish(link.id, "error", "listener_start_failed", error.what());
        }
    }

    void startDefinition(const LinkDefinition& link) {
        if (link.status != "enabled") {
            publish(link.id, "stopped");
            return;
        }
        if (link.mode == "TCP Server")
            listenServers_ ? startServer(link) : publish(link.id, "idle");
        else if (link.mode == "TCP Client")
            startClients(link);
        else
            publish(link.id, "error", "unsupported_link_mode", "不支持的链路模式");
    }

    void startClients(const LinkDefinition& link) {
        bool assigned = false;
        for (const auto& target : link.targets) {
            if (target.status != "enabled" ||
                std::hash<std::string_view>{}(target.id) % workerCount_ != workerIndex_)
                continue;
            assigned = true;
            auto client = std::make_shared<ClientTarget>(*this, link, target);
            clients_.emplace(client->key(), client);
            client->start();
        }
        if (!assigned)
            publish(link.id, "idle");
    }

    void addConnection(const LinkDefinition& link, std::string targetId, std::string id,
                       std::string remote, asio::ip::tcp::socket socket) {
        const auto connectionId = id;
        const auto sessionEpoch = ++sessionEpoch_;
        connectionEpochs_[connectionId] = sessionEpoch;
        auto connection = std::make_shared<Connection>(
            std::move(socket), id, remote,
            [this, linkId = link.id](std::string_view currentId, std::string_view currentRemote,
                                     std::vector<std::uint8_t> bytes) {
                auto& activity = lastActivity_[linkId];
                activity = message::utcNowMilliseconds();
                const auto targetId = connectionTargets_.find(std::string(currentId));
                if (targetId != connectionTargets_.end() && !targetId->second.empty()) {
                    const auto client = clients_.find(linkId + ':' + targetId->second);
                    if (client != clients_.end())
                        client->second->lastActivityAtMs = activity;
                }
                message::IngressPacket packet{.messageId = message::nextMessageId(),
                                             .linkId = linkId,
                                             .connectionId = std::string(currentId),
                                             .remoteAddress = std::string(currentRemote),
                                             .sessionEpoch = connectionEpochs_.at(
                                                 std::string(currentId)),
                                             .occurredAtMs = activity,
                                             .payload = std::move(bytes)};
                onPacket_(std::move(packet));
                publish(linkId, definitions_[linkId].mode == "TCP Server" ? "listening"
                                                                           : "connected",
                        {}, {}, false);
            },
            [this](std::string_view currentId, std::string_view reason) {
                removeConnection(currentId, reason, true);
            });
        connections_.emplace(connectionId, connection);
        connectionLinks_[connectionId] = link.id;
        connectionTargets_[connectionId] = targetId;
        onConnected_({.connectionId = connectionId,
                      .linkId = link.id,
                      .remoteAddress = remote,
                      .targetId = std::move(targetId),
                      .sessionEpoch = sessionEpoch});
        publish(link.id, link.mode == "TCP Server" ? "listening" : "connected");
        connection->start();
    }

    void removeConnection(std::string_view id, std::string_view reason, bool notifyClient) {
        const auto current = connections_.find(id);
        if (current == connections_.end())
            return;
        const auto linkId = connectionLinks_[current->first];
        const auto targetId = connectionTargets_[current->first];
        connections_.erase(current);
        connectionLinks_.erase(std::string(id));
        connectionTargets_.erase(std::string(id));
        onDisconnected_(std::string(id), std::string(reason));
        connectionEpochs_.erase(std::string(id));
        publish(linkId, definitions_[linkId].mode == "TCP Server" ? "listening" : "reconnecting",
                reason);
        if (notifyClient && !targetId.empty()) {
            const auto client = clients_.find(linkId + ':' + targetId);
            if (client != clients_.end()) {
                client->second->connectionId.clear();
                client->second->failed = true;
                client->second->reconnectToken = scheduler_.scheduleAfter(
                    std::chrono::seconds(2), [target = client->second] {
                        target->reconnectToken = 0;
                        target->connect();
                    });
            }
        }
    }

    void publish(std::string_view linkId, std::string state, std::string_view reason = {},
                 std::string_view error = {}, bool emit = true) {
        if (!onState_)
            return;
        LinkState status{.linkId = std::string(linkId),
                               .workerIndex = workerIndex_,
                               .state = std::move(state),
                               .reason = std::string(reason),
                               .error = std::string(error),
                               .lastActivityAtMs = lastActivity_[std::string(linkId)]};
        for (const auto& [id, connection] : connections_) {
            const auto owner = connectionLinks_.find(id);
            if (owner != connectionLinks_.end() && owner->second == linkId)
                status.remoteEndpoints.push_back(connection->remote());
        }
        for (const auto& [key, client] : clients_) {
            (void)key;
            if (client->link.id != linkId)
                continue;
            status.targets.push_back({.id = client->target.id,
                                      .state = client->state,
                                      .reason = client->reason,
                                      .error = client->error,
                                      .lastActivityAtMs = client->lastActivityAtMs});
        }
        (void)emit;
        onState_(std::move(status));
    }

    void stopTransports() noexcept {
        for (auto& [id, listener] : listeners_) {
            (void)id;
            listener->stop();
        }
        listeners_.clear();
        for (auto& [id, client] : clients_) {
            (void)id;
            client->stop();
        }
        clients_.clear();
        while (!connections_.empty())
            connections_.begin()->second->close("local_closed");
        connectionLinks_.clear();
        connectionTargets_.clear();
    }

    asio::io_context& ioContext_;
    Timer& scheduler_;
    std::size_t workerIndex_ = 0;
    std::size_t workerCount_ = 1;
    ConnectedHandler onConnected_;
    PacketHandler onPacket_;
    DisconnectedHandler onDisconnected_;
    StateHandler onState_;
    std::map<std::string, LinkDefinition, std::less<>> definitions_;
    std::map<std::string, std::shared_ptr<Listener>, std::less<>> listeners_;
    std::map<std::string, std::shared_ptr<ClientTarget>, std::less<>> clients_;
    std::map<std::string, std::shared_ptr<Connection>, std::less<>> connections_;
    std::map<std::string, std::string, std::less<>> connectionLinks_;
    std::map<std::string, std::string, std::less<>> connectionTargets_;
    std::map<std::string, std::uint64_t, std::less<>> connectionEpochs_;
    std::map<std::string, std::int64_t, std::less<>> lastActivity_;
    std::uint64_t sessionEpoch_ = 0;
    bool stopped_ = true;
    bool listenServers_ = false;
    ServerAcceptHandler onServerAccept_;
};

} // namespace service::collector
