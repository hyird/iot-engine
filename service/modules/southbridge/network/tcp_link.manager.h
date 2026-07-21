#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/protocol/protocol_dispatcher.h"
#include "service/modules/southbridge/queue/redis_stream.h"
#include "service/modules/southbridge/runtime.types.h"

namespace service::southbridge {

class TcpConnection final : public std::enable_shared_from_this<TcpConnection> {
  public:
    using DataHandler =
        std::function<void(std::string_view, std::string_view, std::vector<std::uint8_t>)>;
    using CloseHandler = std::function<void(std::string_view)>;
    using WriteHandler = std::function<void(bool)>;

    TcpConnection(asio::ip::tcp::socket socket, std::string connectionId, std::string remoteAddress,
                  DataHandler onData, CloseHandler onClose)
        : socket_(std::move(socket)), connectionId_(std::move(connectionId)),
          remoteAddress_(std::move(remoteAddress)), onData_(std::move(onData)),
          onClose_(std::move(onClose)) {}

    [[nodiscard]] const std::string& id() const noexcept { return connectionId_; }
    [[nodiscard]] const std::string& remoteAddress() const noexcept { return remoteAddress_; }

    void start() { read(); }

    void close() {
        asio::post(socket_.get_executor(), [self = shared_from_this()] { self->finish(); });
    }

    void send(std::vector<std::uint8_t> bytes, WriteHandler handler) {
        asio::post(socket_.get_executor(), [self = shared_from_this(), bytes = std::move(bytes),
                                            handler = std::move(handler)]() mutable {
            if (self->closed_.load()) {
                handler(false);
                return;
            }
            auto payload = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
            asio::async_write(self->socket_, asio::buffer(*payload),
                              [self, payload, handler = std::move(handler)](
                                  const std::error_code& error, std::size_t) mutable {
                                  handler(!error);
                                  if (error)
                                      self->finish();
                              });
        });
    }

  private:
    void read() {
        socket_.async_read_some(asio::buffer(buffer_), [self = shared_from_this()](
                                                           const std::error_code& error,
                                                           std::size_t size) {
            if (error) {
                self->finish();
                return;
            }
            std::vector<std::uint8_t> bytes(
                self->buffer_.begin(), self->buffer_.begin() + static_cast<std::ptrdiff_t>(size));
            self->onData_(self->connectionId_, self->remoteAddress_, std::move(bytes));
            self->read();
        });
    }

    void finish() {
        if (closed_.exchange(true))
            return;
        std::error_code ignored;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        onClose_(connectionId_);
    }

    asio::ip::tcp::socket socket_;
    std::string connectionId_;
    std::string remoteAddress_;
    DataHandler onData_;
    CloseHandler onClose_;
    std::array<std::uint8_t, 8192> buffer_{};
    std::atomic_bool closed_{false};
};

class TcpListener final : public std::enable_shared_from_this<TcpListener> {
  public:
    using AcceptHandler =
        std::function<void(std::string, asio::ip::tcp::socket, std::string remoteAddress)>;

    TcpListener(asio::io_context& ioContext, LinkDefinition definition, AcceptHandler onAccept)
        : acceptor_(ioContext), definition_(std::move(definition)), onAccept_(std::move(onAccept)) {
        const auto address =
            asio::ip::make_address(definition_.ip.empty() ? "0.0.0.0" : definition_.ip);
        const asio::ip::tcp::endpoint endpoint(address, definition_.port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(asio::socket_base::max_listen_connections);
    }

    [[nodiscard]] const std::string& linkId() const noexcept { return definition_.id; }

    void start() { accept(); }

    void stop() {
        std::error_code ignored;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
    }

  private:
    void accept() {
        acceptor_.async_accept([self = shared_from_this()](const std::error_code& error,
                                                           asio::ip::tcp::socket socket) {
            if (!error) {
                std::string remoteAddress;
                std::error_code endpointError;
                const auto endpoint = socket.remote_endpoint(endpointError);
                if (!endpointError)
                    remoteAddress =
                        endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
                self->onAccept_(self->definition_.id, std::move(socket), std::move(remoteAddress));
            }
            if (self->acceptor_.is_open())
                self->accept();
        });
    }

    asio::ip::tcp::acceptor acceptor_;
    LinkDefinition definition_;
    AcceptHandler onAccept_;
};

class TcpClientTarget final : public std::enable_shared_from_this<TcpClientTarget> {
  public:
    using DataHandler =
        std::function<void(std::string_view, std::string_view, std::vector<std::uint8_t>)>;
    using ConnectionHandler =
        std::function<void(std::string_view, std::string_view, bool connected)>;
    using StateHandler = std::function<void(std::string_view, std::string_view, std::string_view)>;
    using WriteHandler = std::function<void(bool)>;

    TcpClientTarget(asio::io_context& ioContext, std::string linkId,
                    LinkTargetDefinition definition, DataHandler onData,
                    ConnectionHandler onConnection, StateHandler onState)
        : ioContext_(ioContext), socket_(ioContext), reconnectTimer_(ioContext), linkId_(linkId),
          definition_(std::move(definition)), connectionId_(linkId_ + "-target-" + definition_.id),
          onData_(std::move(onData)), onConnection_(std::move(onConnection)),
          onState_(std::move(onState)) {}

    void start() { connect(); }

    void stop() {
        if (stopped_.exchange(true))
            return;
        std::error_code ignored;
        reconnectTimer_.cancel(ignored);
        socket_.cancel(ignored);
        socket_.close(ignored);
        if (connected_.exchange(false))
            onConnection_(connectionId_, endpoint(), false);
        onState_("stopped", {}, {});
    }

    void send(std::vector<std::uint8_t> bytes, WriteHandler handler) {
        asio::post(ioContext_, [self = shared_from_this(), bytes = std::move(bytes),
                                handler = std::move(handler)]() mutable {
            if (!self->connected_.load()) {
                handler(false);
                return;
            }
            auto payload = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
            asio::async_write(self->socket_, asio::buffer(*payload),
                              [self, payload, handler = std::move(handler)](
                                  const std::error_code& error, std::size_t) mutable {
                                  handler(!error);
                                  if (error)
                                      self->failed(error);
                              });
        });
    }

  private:
    [[nodiscard]] std::string endpoint() const {
        return definition_.ip + ':' + std::to_string(definition_.port);
    }

    void connect() {
        if (stopped_.load())
            return;
        onState_(hasFailed_ ? "reconnecting" : "connecting", {}, {});
        std::error_code ignored;
        socket_.close(ignored);
        socket_ = asio::ip::tcp::socket(ioContext_);
        std::error_code addressError;
        const auto address = asio::ip::make_address(definition_.ip, addressError);
        if (addressError) {
            failed(addressError);
            return;
        }
        socket_.async_connect({address, definition_.port},
                              [self = shared_from_this()](const std::error_code& error) {
                                  if (error) {
                                      self->failed(error);
                                      return;
                                  }
                                  self->connected_.store(true);
                                  self->onState_("connected", {}, {});
                                  self->onConnection_(self->connectionId_, self->endpoint(), true);
                                  self->read();
                              });
    }

    void read() {
        socket_.async_read_some(asio::buffer(buffer_), [self = shared_from_this()](
                                                           const std::error_code& error,
                                                           std::size_t size) {
            if (error) {
                self->failed(error);
                return;
            }
            std::vector<std::uint8_t> bytes(
                self->buffer_.begin(), self->buffer_.begin() + static_cast<std::ptrdiff_t>(size));
            self->onData_(self->connectionId_, self->endpoint(), std::move(bytes));
            self->read();
        });
    }

    void failed(const std::error_code& error) {
        if (stopped_.load())
            return;
        std::error_code ignored;
        socket_.close(ignored);
        if (connected_.exchange(false))
            onConnection_(connectionId_, endpoint(), false);
        const auto [reason, message, retry] = classify(error);
        hasFailed_ = true;
        onState_(retry ? "reconnecting" : "error", reason, message);
        if (!retry)
            return;
        reconnectTimer_.expires_after(std::chrono::seconds(2));
        reconnectTimer_.async_wait([self = shared_from_this()](const std::error_code& timerError) {
            if (!timerError)
                self->connect();
        });
    }

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
        return {"transport_error", "链路传输异常", true};
    }

    asio::io_context& ioContext_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer reconnectTimer_;
    std::string linkId_;
    LinkTargetDefinition definition_;
    std::string connectionId_;
    DataHandler onData_;
    ConnectionHandler onConnection_;
    StateHandler onState_;
    std::array<std::uint8_t, 8192> buffer_{};
    std::atomic_bool connected_{false};
    std::atomic_bool stopped_{false};
    bool hasFailed_ = false;
};

class TcpLinkManager {
  public:
    TcpLinkManager(bridge::RedisStreamProducer& producer, ProtocolDispatcher& dispatcher)
        : work_(asio::make_work_guard(ioContext_)), producer_(producer), dispatcher_(dispatcher) {
        dispatcher_.setDisplacedConnectionHandler(
            [this](std::string_view connectionId) { close(connectionId); });
    }

    TcpLinkManager(const TcpLinkManager&) = delete;
    TcpLinkManager& operator=(const TcpLinkManager&) = delete;

    ~TcpLinkManager() { stop(); }

    void start(const RuntimeSnapshot& snapshot) {
        if (running_.exchange(true))
            return;
        apply(snapshot);
        ioThread_ = std::thread([this] { ioContext_.run(); });
    }

    void reload(RuntimeSnapshot snapshot) {
        asio::post(ioContext_, [this, snapshot = std::move(snapshot)] { apply(snapshot); });
    }

    void stop() {
        if (!running_.exchange(false))
            return;
        asio::post(ioContext_, [this] {
            for (auto& [id, listener] : listeners_) {
                (void)id;
                listener->stop();
            }
            listeners_.clear();
            for (auto& [key, target] : clientTargets_) {
                (void)key;
                target->stop();
            }
            clientTargets_.clear();
            std::vector<std::shared_ptr<TcpConnection>> connections;
            {
                std::lock_guard lock(connectionMutex_);
                for (const auto& [id, connection] : connections_) {
                    (void)id;
                    connections.push_back(connection);
                }
            }
            for (const auto& connection : connections)
                connection->close();
            for (const auto& [id, definition] : definitions_) {
                (void)id;
                publishStopped(definition);
            }
            work_.reset();
        });
        if (ioThread_.joinable())
            ioThread_.join();
        ioContext_.stop();
    }

    void close(std::string_view connectionId) {
        std::shared_ptr<TcpConnection> connection;
        {
            std::lock_guard lock(connectionMutex_);
            const auto current = connections_.find(std::string(connectionId));
            if (current != connections_.end())
                connection = current->second;
        }
        if (connection)
            connection->close();
    }

    [[nodiscard]] std::future<bool> send(std::string connectionId,
                                         std::vector<std::uint8_t> bytes) {
        auto completion = std::make_shared<std::promise<bool>>();
        auto result = completion->get_future();
        asio::post(ioContext_, [this, connectionId = std::move(connectionId),
                                bytes = std::move(bytes), completion]() mutable {
            const auto finish = [completion](bool sent) {
                try {
                    completion->set_value(sent);
                } catch (...) {
                }
            };
            std::shared_ptr<TcpConnection> connection;
            {
                std::lock_guard lock(connectionMutex_);
                const auto current = connections_.find(connectionId);
                if (current != connections_.end())
                    connection = current->second;
            }
            if (connection) {
                connection->send(std::move(bytes), finish);
                return;
            }
            const auto targetKey = targetConnectionIds_.find(connectionId);
            if (targetKey == targetConnectionIds_.end()) {
                finish(false);
                return;
            }
            const auto target = clientTargets_.find(targetKey->second);
            if (target == clientTargets_.end()) {
                finish(false);
                return;
            }
            target->second->send(std::move(bytes), finish);
        });
        return result;
    }

  private:
    struct TargetStatus {
        std::string state = "connecting";
        std::string reason;
        std::string error;
        std::int64_t lastActivityAtMs = 0;
    };

    void apply(const RuntimeSnapshot& snapshot) {
        const auto previousDefinitions = definitions_;
        for (auto& [id, listener] : listeners_) {
            (void)id;
            listener->stop();
        }
        listeners_.clear();
        for (auto& [key, target] : clientTargets_) {
            (void)key;
            target->stop();
        }
        clientTargets_.clear();
        std::vector<std::shared_ptr<TcpConnection>> connections;
        {
            std::lock_guard lock(connectionMutex_);
            for (const auto& [id, connection] : connections_) {
                (void)id;
                connections.push_back(connection);
            }
        }
        for (const auto& connection : connections)
            connection->close();

        definitions_.clear();
        targetStatuses_.clear();
        targetConnectionIds_.clear();
        serverConnectionIds_.clear();
        linkErrors_.clear();
        for (const auto& link : snapshot.links)
            definitions_[link.id] = link;
        for (const auto& [id, definition] : previousDefinitions)
            if (!definitions_.contains(id))
                safeRemoveStatus(id);

        for (const auto& link : snapshot.links) {
            if (link.status != "enabled") {
                publishStopped(link);
                continue;
            }
            if (link.mode == "TCP Client") {
                startClient(link);
                continue;
            }
            if (link.mode != "TCP Server") {
                linkErrors_[link.id] = "unsupported link mode";
                publishStatus(link.id);
                continue;
            }
            try {
                auto listener = std::make_shared<TcpListener>(
                    ioContext_, link,
                    [this](std::string linkId, asio::ip::tcp::socket socket,
                           std::string remoteAddress) {
                        accepted(linkId, std::move(socket), std::move(remoteAddress));
                    });
                listener->start();
                listeners_.emplace(link.id, std::move(listener));
                publishStatus(link.id);
                std::cout << "southbridge link listening: id=" << link.id << " " << link.ip << ':'
                          << link.port << '\n';
            } catch (const std::exception& error) {
                linkErrors_[link.id] = error.what();
                publishStatus(link.id);
                std::cerr << "southbridge link start failed: id=" << link.id
                          << " error=" << error.what() << '\n';
            }
        }
    }

    void startClient(const LinkDefinition& link) {
        bool hasEnabledTarget = false;
        for (const auto& target : link.targets) {
            if (target.status != "enabled")
                continue;
            hasEnabledTarget = true;
            const auto key = targetKey(link.id, target.id);
            targetStatuses_[key] = {};
            const auto connectionId = link.id + "-target-" + target.id;
            targetConnectionIds_[connectionId] = key;
            auto runtime = std::make_shared<TcpClientTarget>(
                ioContext_, link.id, target,
                [this, linkId = link.id](std::string_view id, std::string_view remote,
                                         std::vector<std::uint8_t> bytes) {
                    enqueue(linkId, id, remote, std::move(bytes));
                },
                [this, linkId = link.id, targetId = target.id](
                    std::string_view id, std::string_view remote, bool connected) {
                    try {
                        if (connected)
                            dispatcher_.onConnected(std::string(id), linkId, std::string(remote),
                                                    targetId);
                        else
                            dispatcher_.onDisconnected(id);
                    } catch (const std::exception& error) {
                        std::cerr << "southbridge client session event failed: " << error.what()
                                  << '\n';
                    }
                },
                [this, linkId = link.id, key](std::string_view state, std::string_view reason,
                                              std::string_view error) {
                    auto& status = targetStatuses_[key];
                    status.state = state;
                    status.reason = reason;
                    status.error = error;
                    if (running_.load())
                        publishStatus(linkId);
                });
            clientTargets_[key] = runtime;
        }
        if (!hasEnabledTarget) {
            publishStopped(link);
            return;
        }
        publishStatus(link.id);
        for (const auto& [key, target] : clientTargets_)
            if (key.starts_with(link.id + ':'))
                target->start();
    }

    void accepted(std::string linkId, asio::ip::tcp::socket socket, std::string remoteAddress) {
        const auto connectionId = linkId + "-" + std::to_string(connectionSequence_.fetch_add(1));
        auto connection = std::make_shared<TcpConnection>(
            std::move(socket), connectionId, remoteAddress,
            [this, linkId](std::string_view id, std::string_view remote,
                           std::vector<std::uint8_t> bytes) {
                enqueue(linkId, id, remote, std::move(bytes));
            },
            [this, linkId](std::string_view id) {
                {
                    std::lock_guard lock(connectionMutex_);
                    connections_.erase(std::string(id));
                }
                serverConnectionIds_[linkId].erase(std::string(id));
                try {
                    dispatcher_.onDisconnected(id);
                } catch (const std::exception& error) {
                    std::cerr << "southbridge disconnect event failed: " << error.what() << '\n';
                }
                if (running_.load())
                    publishStatus(linkId);
            });
        {
            std::lock_guard lock(connectionMutex_);
            connections_[connectionId] = connection;
        }
        serverConnectionIds_[linkId].insert(connectionId);
        try {
            dispatcher_.onConnected(connectionId, linkId, remoteAddress);
        } catch (const std::exception& error) {
            std::cerr << "southbridge connect event failed: " << error.what() << '\n';
            connection->close();
            return;
        }
        publishStatus(linkId);
        connection->start();
    }

    void enqueue(std::string linkId, std::string_view id, std::string_view remote,
                 std::vector<std::uint8_t> bytes) {
        const auto now = bridge::utcNowMilliseconds();
        linkLastActivityAtMs_[linkId] = now;
        const auto target = targetConnectionIds_.find(std::string(id));
        if (target != targetConnectionIds_.end())
            targetStatuses_[target->second].lastActivityAtMs = now;
        bridge::IngressPacket packet;
        packet.messageId = bridge::nextMessageId();
        packet.linkId = linkId;
        packet.connectionId = id;
        packet.remoteAddress = remote;
        packet.occurredAtMs = now;
        packet.payload = std::move(bytes);
        try {
            producer_.publish(bridge::kIngressStream, bridge::ingressFields(packet));
            publishStatus(linkId, false);
        } catch (const std::exception& error) {
            std::cerr << "southbridge ingress enqueue failed: " << error.what() << '\n';
        }
    }

    void publishStatus(std::string linkId, bool publishEvent = true) {
        const auto current = definitions_.find(linkId);
        if (current == definitions_.end())
            return;
        const auto& definition = current->second;
        std::int64_t connectionCount = 0;
        std::int64_t enabledTargetCount = 0;
        std::string state;
        std::string reason;
        std::string error = linkErrors_[linkId];
        if (!error.empty())
            reason = "listener_start_failed";
        std::string clients;
        std::vector<bridge::StreamField> fields;
        if (definition.mode == "TCP Server") {
            state = listeners_.contains(linkId) ? "listening" : "error";
            std::lock_guard lock(connectionMutex_);
            for (const auto& connectionId : serverConnectionIds_[linkId]) {
                const auto connection = connections_.find(connectionId);
                if (connection == connections_.end())
                    continue;
                if (!clients.empty())
                    clients.push_back('\n');
                clients += connection->second->remoteAddress();
                ++connectionCount;
            }
        } else {
            bool hasError = false;
            bool hasReconnecting = false;
            for (const auto& target : definition.targets) {
                if (target.status != "enabled")
                    continue;
                ++enabledTargetCount;
                const auto key = targetKey(linkId, target.id);
                const auto status = targetStatuses_.find(key);
                const auto targetState = status == targetStatuses_.end() ? std::string("connecting")
                                                                         : status->second.state;
                if (targetState == "connected") {
                    ++connectionCount;
                    if (!clients.empty())
                        clients.push_back('\n');
                    clients += target.ip + ':' + std::to_string(target.port);
                }
                if (targetState == "error")
                    hasError = true;
                if (targetState == "reconnecting")
                    hasReconnecting = true;
                if (reason.empty() && status != targetStatuses_.end() &&
                    !status->second.reason.empty())
                    reason = status->second.reason;
                if (error.empty() && status != targetStatuses_.end() &&
                    !status->second.error.empty())
                    error = status->second.error;
                fields.push_back({"target:" + target.id + ":state", targetState});
                fields.push_back({"target:" + target.id + ":reason", status == targetStatuses_.end()
                                                                         ? std::string{}
                                                                         : status->second.reason});
                fields.push_back({"target:" + target.id + ":error", status == targetStatuses_.end()
                                                                        ? std::string{}
                                                                        : status->second.error});
                fields.push_back({"target:" + target.id + ":last_activity_at_ms",
                                  std::to_string(status == targetStatuses_.end()
                                                     ? 0
                                                     : status->second.lastActivityAtMs)});
            }
            if (enabledTargetCount == 0)
                state = "stopped";
            else if (connectionCount == enabledTargetCount)
                state = "connected";
            else if (connectionCount > 0)
                state = "partial";
            else if (hasReconnecting)
                state = "reconnecting";
            else
                state = hasError ? "error" : "connecting";
        }
        fields.insert(
            fields.begin(),
            {{"mode", definition.mode},
             {"link_name", definition.name},
             {"protocol", definition.protocol},
             {"state", state},
             {"state_reason", reason},
             {"connection_count", std::to_string(connectionCount)},
             {"enabled_target_count", std::to_string(enabledTargetCount)},
             {"local_endpoint", definition.mode == "TCP Server"
                                    ? definition.ip + ':' + std::to_string(definition.port)
                                    : std::string{}},
             {"clients", clients},
             {"error", error},
             {"last_activity_at_ms", std::to_string(linkLastActivityAtMs_[linkId])},
             {"updated_at_ms", std::to_string(bridge::utcNowMilliseconds())}});
        try {
            producer_.writeLinkStatus(linkId, fields, publishEvent);
        } catch (const std::exception& exception) {
            std::cerr << "southbridge link status publish failed: id=" << linkId
                      << " error=" << exception.what() << '\n';
        }
    }

    void publishStopped(const LinkDefinition& definition) {
        try {
            producer_.writeLinkStatus(
                definition.id, {{"link_name", definition.name},
                                {"protocol", definition.protocol},
                                {"mode", definition.mode},
                                {"state", "stopped"},
                                {"state_reason", ""},
                                {"connection_count", "0"},
                                {"enabled_target_count", "0"},
                                {"local_endpoint", ""},
                                {"clients", ""},
                                {"error", ""},
                                {"last_activity_at_ms", "0"},
                                {"updated_at_ms", std::to_string(bridge::utcNowMilliseconds())}});
        } catch (const std::exception& error) {
            std::cerr << "southbridge stopped status publish failed: id=" << definition.id
                      << " error=" << error.what() << '\n';
        }
    }

    void safeRemoveStatus(std::string linkId) {
        try {
            producer_.removeLinkStatus(linkId);
        } catch (const std::exception& error) {
            std::cerr << "southbridge link status removal failed: id=" << linkId
                      << " error=" << error.what() << '\n';
        }
    }

    static std::string targetKey(std::string_view linkId, std::string_view targetId) {
        return std::string(linkId) + ':' + std::string(targetId);
    }

    asio::io_context ioContext_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    bridge::RedisStreamProducer& producer_;
    ProtocolDispatcher& dispatcher_;
    std::map<std::string, std::shared_ptr<TcpListener>> listeners_;
    std::map<std::string, std::shared_ptr<TcpClientTarget>> clientTargets_;
    std::map<std::string, std::shared_ptr<TcpConnection>> connections_;
    std::map<std::string, LinkDefinition> definitions_;
    std::map<std::string, std::set<std::string>> serverConnectionIds_;
    std::map<std::string, TargetStatus> targetStatuses_;
    std::map<std::string, std::string> targetConnectionIds_;
    std::map<std::string, std::string> linkErrors_;
    std::map<std::string, std::int64_t> linkLastActivityAtMs_;
    std::mutex connectionMutex_;
    std::thread ioThread_;
    std::atomic_bool running_{false};
    std::atomic_uint64_t connectionSequence_{1};
};

} // namespace service::southbridge
