#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <thread>

#include <asio.hpp>
#include <ruvia/core/OneShot.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/modules/system/outbox/outbox.service.h"
#include "service/modules/edge_node/edge_node.service.h"
#include <ruvia/web/ModelJson.h>
#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/modules/link/link.types.h"
#include "service/modules/link/link.entity.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/user/user.entity.h"

namespace service::link {

// 北桥自持的异步 HTTP（出站）worker：拥有自己的 io_context/线程（属于北桥，与南桥完全独立，
// 不共享 io_context、不破坏南北向边界）。全异步 socket（sans-io，HTTP 报文自解析），worker
// 协程经 OneShot 桥接，从不阻塞 ruvia worker。一个常驻 io 线程（全程复用，非 per-request）。
class OutboundHttp {
  public:
    OutboundHttp() : work_(asio::make_work_guard(io_)), thread_([this] { io_.run(); }) {}
    ~OutboundHttp() {
        work_.reset();
        io_.stop();
        if (thread_.joinable())
            thread_.join();
    }
    OutboundHttp(const OutboundHttp&) = delete;
    OutboundHttp& operator=(const OutboundHttp&) = delete;

    // 全异步 GET；完成时（在自有 io 线程上）回调 onDone(响应原文；失败为空串)。
    void get(std::string host, std::string port, std::string request,
             std::function<void(std::string)> onDone) {
        asio::co_spawn(
            io_,
            [host = std::move(host), port = std::move(port),
             request = std::move(request)]() -> asio::awaitable<std::string> {
                auto executor = co_await asio::this_coro::executor;
                asio::ip::tcp::resolver resolver(executor);
                asio::ip::tcp::socket socket(executor);
                const auto endpoints =
                    co_await resolver.async_resolve(host, port, asio::use_awaitable);
                co_await asio::async_connect(socket, endpoints, asio::use_awaitable);
                co_await asio::async_write(socket, asio::buffer(request), asio::use_awaitable);
                std::string response;
                std::array<char, 4096> buffer{};
                for (;;) {
                    std::error_code readError;
                    const auto size = co_await socket.async_read_some(
                        asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, readError));
                    if (readError || size == 0)
                        break;
                    response.append(buffer.data(), size);
                }
                co_return response;
            },
            [onDone = std::move(onDone)](std::exception_ptr error, std::string response) {
                onDone(error ? std::string{} : std::move(response));
            });
    }

  private:
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread thread_;
};

class LinkService {
  public:
    static LinkService& instance() {
        static thread_local LinkService service;
        return service;
    }

    ruvia::Task<LinkPageDataDto> list(ruvia::Context& c, std::int64_t page, std::int64_t pageSize,
                                      std::optional<std::string> keyword,
                                      std::optional<std::string> mode,
                                      std::optional<std::string> protocol,
                                      std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        ruvia::DbQuery count(c.pool());
        count.select(count.aggregate("count", {count.star()})).from(LinkEntity::tableName());
        applyFilters(count, keyword, mode, protocol, status);
        const auto countRows = co_await c.db().query(count);
        const ruvia::Int64 total = countRows.empty()
                                       ? ruvia::Int64{0}
                                       : toInt(countRows.front()[0].value().value_or(std::string_view{}));

        auto query = linkSelect(c.pool());
        applyFilters(query, keyword, mode, protocol, status);
        query.orderBy(query.column("id"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>((page - 1) * pageSize));
        const auto rows = co_await c.db().query(query);

        ruvia::BoxedArray<LinkItemDto> links(ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = links.emplace(ruvia::ModelOptions{.resource = c.arena()});
            co_await fill(c, item, row);
        }
        LinkPageDataDto result(ruvia::ModelOptions{.resource = c.arena()});
        result.set<"list">(std::move(links))
            .set<"total">(total)
            .set<"page">(page)
            .set<"pageSize">(pageSize)
            .set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    ruvia::Task<LinkItemDto> detail(ruvia::Context& c, std::string_view id) {
        auto query = linkSelect(c.pool());
        query.where((LinkEntity::column<"id">() == id &&
                     LinkEntity::column<"deleted_at">().isNull())
                        .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(15001, "链路不存在", 404);
        LinkItemDto item(ruvia::ModelOptions{.resource = c.arena()});
        co_await fill(c, item, rows.front());
        co_return item;
    }

    ruvia::Task<ruvia::BoxedArray<LinkOptionDto>> options(ruvia::Context& c) {
        auto query = linkOptionSelect(c.pool());
        query.where((LinkEntity::column<"deleted_at">().isNull() &&
                     LinkEntity::column<"status">() == "enabled")
                        .expression(query))
            .orderBy(query.column("name"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<LinkOptionDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            LinkEndpointDto endpoint(ruvia::ModelOptions{.resource = c.arena()});
            endpoint.set<"mode">(row[3].value().value_or(std::string_view{}))
                .set<"ip">(row[4].value().value_or(std::string_view{}))
                .set<"port">(toInt(row[5].value().value_or(std::string_view{})))
                .set<"targets">(co_await loadTargets(c, row[0].value().value_or(std::string_view{}), RuntimeStatus{}));
            item.set<"execution">(row[6].value().value_or("collector"));
            item.set<"edgeNodeId">(row[7].value().value_or(""));
            co_await fillEndpoint(c, endpoint, row[0].value().value_or(""));
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"protocol">(row[2].value().value_or(std::string_view{}))
                .set<"endpoint">(std::move(endpoint));
        }
        co_return result;
    }

    LinkEnumsDto enums(ruvia::Context& c) {
        LinkEnumsDto result(ruvia::ModelOptions{.resource = c.arena()});
        const auto modelOptions = ruvia::ModelOptions{.resource = c.arena()};
        ruvia::BoxedArray<ruvia::String> modes(modelOptions);
        modes.emplace("TCP Server", modelOptions);
        modes.emplace("TCP Client", modelOptions);
        ruvia::BoxedArray<ruvia::String> protocols(modelOptions);
        protocols.emplace("SL651", modelOptions);
        protocols.emplace("Modbus", modelOptions);
        protocols.emplace("S7", modelOptions);
        ruvia::BoxedArray<ruvia::String> statuses(modelOptions);
        statuses.emplace("enabled", modelOptions);
        statuses.emplace("disabled", modelOptions);
        result.set<"modes">(std::move(modes))
            .set<"protocols">(std::move(protocols))
            .set<"statuses">(std::move(statuses));
        return result;
    }

    // 全异步：公网 IP 查询走自建异步 HTTP worker（全异步 socket），OneShot 桥回，不阻塞 worker。
    // 命中缓存直接返回（5 分钟）。
    ruvia::Task<std::string> publicIp(ruvia::Context& c) {
        {
            std::lock_guard lock(publicIpMutex_);
            const auto now = std::chrono::steady_clock::now();
            if (!cachedPublicIp_.empty() && now - publicIpCachedAt_ < std::chrono::minutes(5))
                co_return cachedPublicIp_;
        }
        auto [completion, receiver] = ruvia::makeOneShot<std::string>(c.worker());
        auto shared = std::make_shared<ruvia::OneShotCompletion<std::string>>(std::move(completion));
        http_.get("ip.sb", "80",
                  "GET / HTTP/1.1\r\nHost: ip.sb\r\nUser-Agent: curl/8.0\r\n"
                  "Accept: text/plain\r\nConnection: close\r\n\r\n",
                  [shared](std::string response) {
                      (void)shared->complete(parsePublicIp(response));
                  });
        auto result = co_await receiver.wait();
        const std::string resolved =
            result.hasValue() ? std::move(result).takeValue() : std::string{};
        std::lock_guard lock(publicIpMutex_);
        if (!resolved.empty()) {
            cachedPublicIp_ = resolved;
            publicIpCachedAt_ = std::chrono::steady_clock::now();
        }
        co_return cachedPublicIp_;
    }

    ruvia::Task<void> create(ruvia::Context& c, const SaveLinkBody& body) {
        if (body.get<"execution">() && body.get<"execution">()->view() == "edge") {
            co_await saveEdgeChannel(c, {}, body); co_return;
        }
        const auto principal = service::middleware::requireAuth(c);
        const auto name = required(body.get<"name">(), "链路名称不能为空");
        const auto protocol = required(body.get<"protocol">(), "协议不能为空");
        const auto& endpoint = requiredEndpoint(body);
        const auto mode = required(endpoint.get<"mode">(), "链路模式不能为空");
        const auto ip = endpoint.get<"ip">() ? std::string(endpoint.get<"ip">()->view()) : "";
        const auto port = endpoint.get<"port">() ? static_cast<std::int64_t>(*endpoint.get<"port">()) : 0;
        const auto status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        if (status != "enabled" && status != "disabled")
            service::common::fail(15002, "状态无效", 400);
        const auto& targets = requiredTargets(endpoint);
        validateConfiguration(mode, protocol, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::nullopt);
        const auto endpointJson = serializeEndpoint(mode, ip, port, targets);
        const auto id = service::common::nextUuidV7();
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery query(c.pool());
        query.insertInto(LinkEntity::tableName(),
                         {"id", "name", "protocol", "endpoint", "status", "created_by",
                          "execution"})
            .values({query.cast(query.value(id), ruvia::DbDataType::kUuid), query.value(name),
                     query.value(protocol), query.cast(query.value(endpointJson),
                                                       ruvia::DbDataType::kJsonb),
                     query.value(status),
                     query.cast(query.value(principal.userId), ruvia::DbDataType::kUuid),
                     query.value("collector")});
        (void)co_await transaction.execute(query);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "created", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> update(ruvia::Context& c, std::string_view id, const SaveLinkBody& body) {
        if (body.get<"execution">() && body.get<"execution">()->view() == "edge") {
            co_await saveEdgeChannel(c, id, body); co_return;
        }
        ruvia::DbQuery lookup(c.pool());
        lookup
            .select({jsonText(lookup, "endpoint", "mode"), lookup.column("protocol"),
                     lookup.column("created_by")})
            .from(LinkEntity::tableName())
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"deleted_at">().isNull() &&
                    LinkEntity::column<"execution">() == "collector")
                       .expression(lookup))
            .limit(1);
        const auto rows = co_await c.db().query(lookup);
        if (rows.empty())
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, rows.front()[2].value().value_or(std::string_view{}));

        const auto name = required(body.get<"name">(), "链路名称不能为空");
        const auto protocol = required(body.get<"protocol">(), "协议不能为空");
        const auto& endpoint = requiredEndpoint(body);
        const auto mode = required(endpoint.get<"mode">(), "链路模式不能为空");
        if (mode != rows.front()[0].value().value_or(std::string_view{}) || protocol != rows.front()[1].value().value_or(std::string_view{}))
            service::common::fail(15006, "链路模式和协议创建后不能修改", 400);
        const auto ip = endpoint.get<"ip">() ? std::string(endpoint.get<"ip">()->view()) : "";
        const auto port = endpoint.get<"port">() ? static_cast<std::int64_t>(*endpoint.get<"port">()) : 0;
        const auto status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        if (status != "enabled" && status != "disabled")
            service::common::fail(15002, "状态无效", 400);
        const auto& targets = requiredTargets(endpoint);
        validateConfiguration(mode, protocol, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::string(id));
        const auto endpointJson = serializeEndpoint(mode, ip, port, targets);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery query(c.pool());
        query.update(LinkEntity::tableName())
            .set("name", query.value(name))
            .set("endpoint", query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb))
            .set("status", query.value(status))
            .set("updated_at", query.call("now"))
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"deleted_at">().isNull() &&
                    LinkEntity::column<"execution">() == "collector")
                       .expression(query));
        const auto nameChanged = query.binary(query.column("name"),
                                               ruvia::DbBinaryOperator::kIsDistinctFrom,
                                               query.value(name));
        const auto endpointChanged = query.binary(
            query.column("endpoint"), ruvia::DbBinaryOperator::kIsDistinctFrom,
            query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb));
        const auto statusChanged = query.binary(query.column("status"),
                                                ruvia::DbBinaryOperator::kIsDistinctFrom,
                                                query.value(status));
        query.andWhere(query.binary(query.binary(nameChanged, ruvia::DbBinaryOperator::kOr,
                                                  endpointChanged),
                                    ruvia::DbBinaryOperator::kOr, statusChanged));
        const auto updated = co_await transaction.execute(query);
        if (updated.affectedRows() != 0)
            co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "updated", id);
        co_await transaction.commit();
    }

    ruvia::Task<void> remove(ruvia::Context& c, std::string_view id) {
        ruvia::DbQuery lookup(c.pool());
        lookup.select(lookup.column("created_by"))
            .from(LinkEntity::tableName())
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"deleted_at">().isNull())
                       .expression(lookup))
            .limit(1);
        const auto rows = co_await c.db().query(lookup);
        if (rows.empty())
            service::common::fail(15001, "链路不存在", 404);
        co_await requireOwner(c, rows.front()[0].value().value_or(std::string_view{}));
        ruvia::DbQuery used(c.pool());
        used.select(used.cast(used.value(1), ruvia::DbDataType::kInteger))
            .from("device")
            .where(used.binary(used.column("link_id"), ruvia::DbBinaryOperator::kEqual,
                               used.cast(used.value(id), ruvia::DbDataType::kUuid)))
            .andWhere(used.unary(ruvia::DbUnaryOperator::kIsNull,
                                 used.column("deleted_at")))
            .limit(1);
        if (!(co_await c.db().query(used)).empty())
            service::common::fail(15008, "链路已被设备使用，请先删除关联设备", 409);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery removal(c.pool());
        removal.update(LinkEntity::tableName())
            .set("deleted_at", removal.call("now"))
            .set("updated_at", removal.call("now"))
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"deleted_at">().isNull())
                       .expression(removal));
        (void)co_await transaction.execute(removal);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "deleted", id);
        co_await transaction.commit();
    }

  private:
    static ruvia::DbExpression jsonText(ruvia::DbQuery& query, std::string_view column,
                                        std::string_view key, std::string_view table = {}) {
        return query.binary(query.column(column, table), ruvia::DbBinaryOperator::kJsonGetText,
                            query.cast(query.value(key), ruvia::DbDataType::kText));
    }

    static ruvia::DbExpression jsonValue(ruvia::DbQuery& query, std::string_view column,
                                         std::string_view key, std::string_view table = {}) {
        return query.binary(query.column(column, table), ruvia::DbBinaryOperator::kJsonGet,
                            query.cast(query.value(key), ruvia::DbDataType::kText));
    }

    static ruvia::DbQuery linkSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto endpointPort = query.coalesce(
            {query.nullIf(jsonText(query, "endpoint", "port"), query.value("")),
             query.value("0")});
        query.select({query.cast(query.column("id"), ruvia::DbDataType::kText),
                      query.column("name"), query.column("protocol"),
                      jsonText(query, "endpoint", "mode"),
                      query.coalesce({jsonText(query, "endpoint", "ip"), query.value("")}),
                      endpointPort, query.column("status"),
                      query.cast(query.column("created_by"), ruvia::DbDataType::kText),
                      query.call("iot_utc_timestamp", {query.column("created_at")}),
                      query.call("iot_utc_timestamp", {query.column("updated_at")}),
                      query.column("execution"),
                      query.coalesce({query.cast(query.column("edge_node_id"),
                                                 ruvia::DbDataType::kText),
                                      query.value("")})})
            .from(LinkEntity::tableName());
        return query;
    }

    static ruvia::DbQuery linkOptionSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto endpointPort = query.coalesce(
            {query.nullIf(jsonText(query, "endpoint", "port"), query.value("")),
             query.value("0")});
        query.select({query.cast(query.column("id"), ruvia::DbDataType::kText),
                      query.column("name"), query.column("protocol"),
                      jsonText(query, "endpoint", "mode"),
                      query.coalesce({jsonText(query, "endpoint", "ip"), query.value("")}),
                      endpointPort, query.column("execution"),
                      query.coalesce({query.cast(query.column("edge_node_id"),
                                                 ruvia::DbDataType::kText),
                                      query.value("")})})
            .from(LinkEntity::tableName());
        return query;
    }

    static void applyFilters(ruvia::DbQuery& query, const std::optional<std::string>& keyword,
                             const std::optional<std::string>& mode,
                             const std::optional<std::string>& protocol,
                             const std::optional<std::string>& status) {
        query.where(LinkEntity::column<"deleted_at">().isNull().expression(query));
        if (keyword && !keyword->empty())
            query.andWhere(LinkEntity::column<"name">().ilike("%" + *keyword + "%")
                               .expression(query));
        if (mode && !mode->empty())
            query.andWhere(query.binary(jsonText(query, "endpoint", "mode"),
                                        ruvia::DbBinaryOperator::kEqual, query.value(*mode)));
        if (protocol && !protocol->empty())
            query.andWhere((LinkEntity::column<"protocol">() == *protocol).expression(query));
        if (status && !status->empty())
            query.andWhere((LinkEntity::column<"status">() == *status).expression(query));
    }

    static ruvia::Task<void> fillEndpoint(ruvia::Context& c, LinkEndpointDto& endpoint, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({jsonText(query, "endpoint", "transport"),
                      jsonText(query, "endpoint", "interface"),
                      jsonText(query, "endpoint", "baud_rate"),
                      jsonText(query, "endpoint", "data_bits"),
                      jsonText(query, "endpoint", "stop_bits"),
                      jsonText(query, "endpoint", "parity"),
                      jsonText(query, "endpoint", "rs485")})
            .from(LinkEntity::tableName())
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"execution">() == "edge")
                       .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) co_return;
        const auto& row = rows.front();
        endpoint.set<"transport">(row[0].value().value_or(""));
        endpoint.set<"interfaceName">(row[1].value().value_or(""));
        endpoint.set<"baudRate">(toInt(row[2].value().value_or("9600")));
        endpoint.set<"dataBits">(toInt(row[3].value().value_or("8")));
        endpoint.set<"stopBits">(toInt(row[4].value().value_or("1")));
        endpoint.set<"parity">(row[5].value().value_or("none"));
        endpoint.set<"rs485">(row[6].value().value_or("false") == "true");
    }

    ruvia::Task<void> saveEdgeChannel(ruvia::Context& c, std::string_view existingId, const SaveLinkBody& body) {
        const auto principal = service::middleware::requireAuth(c);
        const auto name = required(body.get<"name">(), "通道名称不能为空");
        const auto protocol = required(body.get<"protocol">(), "协议不能为空");
        const auto nodeId = required(body.get<"edgeNodeId">(), "请选择边缘节点");
        if (!service::common::isUuid(nodeId)) service::common::fail(15002, "节点 ID 无效", 400);
        const auto& endpoint = requiredEndpoint(body);
        const auto transport = required(endpoint.get<"transport">(), "请选择传输类型");
        const auto interfaceName = required(endpoint.get<"interfaceName">(), "请选择接口");
        if (interfaceName.size() > 96) service::common::fail(15002, "接口名称过长", 400);
        if (transport == "serial") {
            if (protocol == "S7") service::common::fail(15002, "S7 不支持串口", 400);
            const auto baud = endpoint.get<"baudRate">().value_or(9600);
            const auto bits = endpoint.get<"dataBits">().value_or(8);
            const auto stops = endpoint.get<"stopBits">().value_or(1);
            const auto parity = endpoint.get<"parity">() ? endpoint.get<"parity">()->view() : std::string_view("none");
            if (baud < 300 || baud > 4000000 || bits < 5 || bits > 8 || stops < 1 || stops > 2 ||
                (parity != "none" && parity != "odd" && parity != "even"))
                service::common::fail(15002, "串口参数无效", 400);
        } else if (transport == "tcp") {
            const auto mode = required(endpoint.get<"mode">(), "请选择 TCP 模式");
            const auto ip = required(endpoint.get<"ip">(), "请输入 IP 地址");
            std::error_code error;
            (void)asio::ip::make_address_v4(ip, error);
            const auto port = endpoint.get<"port">().value_or(0);
            if (error || port < 1 || port > 65535 || (mode != "TCP Client" && mode != "TCP Server"))
                service::common::fail(15002, "TCP 参数无效", 400);
        } else service::common::fail(15002, "传输类型无效", 400);
        ruvia::DbQuery nodeQuery(c.pool());
        nodeQuery.select(nodeQuery.cast(nodeQuery.value(1), ruvia::DbDataType::kInteger))
            .from("edge_node")
            .where(nodeQuery.binary(nodeQuery.column("id"), ruvia::DbBinaryOperator::kEqual,
                                   nodeQuery.cast(nodeQuery.value(nodeId),
                                                 ruvia::DbDataType::kUuid)))
            .andWhere(nodeQuery.binary(nodeQuery.column("enrollment_status"),
                                      ruvia::DbBinaryOperator::kEqual,
                                      nodeQuery.value("approved")))
            .andWhere(nodeQuery.binary(jsonText(nodeQuery, "capability", "deviceConfig"),
                                       ruvia::DbBinaryOperator::kEqual,
                                       nodeQuery.value("true")));
        const auto node = co_await c.db().query(nodeQuery);
        if (node.empty()) service::common::fail(15002, "节点未批准或不支持采集配置", 400);
        if (transport == "serial") {
            ruvia::DbQuery serialQuery(c.pool());
            serialQuery.select(serialQuery.cast(serialQuery.value(1), ruvia::DbDataType::kInteger))
                .from("edge_node_serial")
                .where(serialQuery.binary(serialQuery.column("node_id"),
                                         ruvia::DbBinaryOperator::kEqual,
                                         serialQuery.cast(serialQuery.value(nodeId),
                                                         ruvia::DbDataType::kUuid)))
                .andWhere(serialQuery.binary(serialQuery.column("path"),
                                             ruvia::DbBinaryOperator::kEqual,
                                             serialQuery.value(interfaceName)))
                .andWhere(serialQuery.unary(ruvia::DbUnaryOperator::kIsTrue,
                                             serialQuery.column("available")));
            const auto serial = co_await c.db().query(serialQuery);
            if (serial.empty()) service::common::fail(15002, "所选串口不存在或当前不可用", 409);
        } else {
            ruvia::DbQuery networkQuery(c.pool());
            networkQuery.select(networkQuery.column("ipv4"))
                .from("edge_node_interface")
                .where(networkQuery.binary(networkQuery.column("node_id"),
                                           ruvia::DbBinaryOperator::kEqual,
                                           networkQuery.cast(networkQuery.value(nodeId),
                                                             ruvia::DbDataType::kUuid)))
                .andWhere(networkQuery.binary(networkQuery.column("name"),
                                              ruvia::DbBinaryOperator::kEqual,
                                              networkQuery.value(interfaceName)))
                .andWhere(networkQuery.binary(
                    networkQuery.coalesce({networkQuery.column("ipv4"), networkQuery.value("")}),
                    ruvia::DbBinaryOperator::kNotEqual, networkQuery.value("")))
                .limit(1);
            const auto network = co_await c.db().query(networkQuery);
            if (network.empty()) service::common::fail(15002, "所选网口不存在或未上报 IPv4", 409);
            const auto mode = endpoint.get<"mode">()->view();
            const auto ip = endpoint.get<"ip">()->view();
            if ((protocol == "S7" && mode != "TCP Client") || (protocol == "SL651" && mode != "TCP Server"))
                service::common::fail(15002, "协议不支持所选 TCP 模式", 400);
            if (mode == "TCP Server" && ip != "0.0.0.0" && ip != network.front()[0].value().value_or(""))
                service::common::fail(15002, "监听地址必须是所选网口地址", 400);
        }
        std::string priorNode;
        if (!existingId.empty()) {
            ruvia::DbQuery currentQuery(c.pool());
            currentQuery.select({currentQuery.column("created_by"),
                                 currentQuery.cast(currentQuery.column("edge_node_id"),
                                                   ruvia::DbDataType::kText)})
                .from(LinkEntity::tableName())
                .where((LinkEntity::column<"id">() == existingId &&
                        LinkEntity::column<"execution">() == "edge" &&
                        LinkEntity::column<"deleted_at">().isNull())
                           .expression(currentQuery))
                .limit(1);
            const auto current = co_await c.db().query(currentQuery);
            if (current.empty()) service::common::fail(15001, "通道不存在", 404);
            co_await requireOwner(c, current.front()[0].value().value_or(""));
            priorNode = current.front()[1].value().value_or("");
        }
        const auto id = existingId.empty() ? service::common::nextUuidV7() : std::string(existingId);
        std::string endpointJson = "{\"transport\":";
        appendJsonString(endpointJson, transport);
        endpointJson += ",\"interface\":";
        appendJsonString(endpointJson, interfaceName);
        if (transport == "serial") {
            endpointJson += ",\"baud_rate\":" + std::to_string(endpoint.get<"baudRate">().value_or(9600));
            endpointJson += ",\"data_bits\":" + std::to_string(endpoint.get<"dataBits">().value_or(8));
            endpointJson += ",\"stop_bits\":" + std::to_string(endpoint.get<"stopBits">().value_or(1));
            endpointJson += ",\"parity\":";
            appendJsonString(endpointJson, endpoint.get<"parity">() ? endpoint.get<"parity">()->view() : std::string_view("none"));
            endpointJson += endpoint.get<"rs485">().value_or(false) ? ",\"rs485\":true" : ",\"rs485\":false";
        } else {
            endpointJson += ",\"mode\":"; appendJsonString(endpointJson, endpoint.get<"mode">()->view());
            endpointJson += ",\"ip\":"; appendJsonString(endpointJson, endpoint.get<"ip">()->view());
            endpointJson += ",\"port\":" + std::to_string(*endpoint.get<"port">());
        }
        endpointJson += '}';
        const auto status = body.get<"status">() ? body.get<"status">()->view() : std::string_view("enabled");
        auto tx = co_await c.db().beginTransaction();
        if (existingId.empty()) {
            ruvia::DbQuery query(c.pool());
            query.insertInto(LinkEntity::tableName(),
                             {"id", "name", "protocol", "endpoint", "status", "created_by",
                              "execution", "edge_node_id"})
                .values({query.cast(query.value(id), ruvia::DbDataType::kUuid),
                         query.value(name), query.value(protocol),
                         query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb),
                         query.value(status),
                         query.cast(query.value(principal.userId), ruvia::DbDataType::kUuid),
                         query.value("edge"),
                         query.cast(query.value(nodeId), ruvia::DbDataType::kUuid)});
            (void)co_await tx.execute(query);
        } else {
            ruvia::DbQuery query(c.pool());
            query.update(LinkEntity::tableName())
                .set("name", query.value(name))
                .set("protocol", query.value(protocol))
                .set("endpoint", query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb))
                .set("status", query.value(status))
                .set("edge_node_id", query.cast(query.value(nodeId), ruvia::DbDataType::kUuid))
                .set("updated_at", query.call("now"))
                .where((LinkEntity::column<"id">() == id &&
                        LinkEntity::column<"execution">() == "edge" &&
                        LinkEntity::column<"deleted_at">().isNull())
                           .expression(query));
            (void)co_await tx.execute(query);
        }
        co_await service::system::OutboxService::enqueueConfigEvent(tx,"link",existingId.empty()?"created":"updated",id);
        co_await tx.commit();
        (void)co_await service::edge::EdgeService::queueSnapshot(c,nodeId);
        if (!priorNode.empty() && priorNode != nodeId)
            (void)co_await service::edge::EdgeService::queueSnapshot(c,priorNode);
    }

    struct RuntimeStatus {
        std::map<std::string, std::string> fields;

        [[nodiscard]] std::string text(std::string_view name,
                                       std::string_view fallback = {}) const {
            const auto current = fields.find(std::string(name));
            return current == fields.end() ? std::string(fallback) : current->second;
        }

        [[nodiscard]] std::int64_t integer(std::string_view name) const {
            const auto value = text(name);
            return service::common::parseInt64(std::optional<std::string_view>{value})
                .value_or(0);
        }
    };

    static ruvia::Int64 toInt(std::string_view value) {
        return static_cast<ruvia::Int64>(
            service::common::parseInt64(std::optional<std::string_view>{value}).value_or(0));
    }

    // sans-io：从 HTTP 响应原文解析公网 IP（校验 200、取 body、trim、字符白名单）。
    static std::string parsePublicIp(const std::string& response) {
        const auto statusEnd = response.find("\r\n");
        const auto headerEnd = response.find("\r\n\r\n");
        if (statusEnd == std::string::npos || headerEnd == std::string::npos ||
            response.substr(0, statusEnd).find(" 200 ") == std::string::npos)
            return {};
        std::string value = response.substr(headerEnd + 4);
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
        if (value.size() > 64 ||
            !std::all_of(value.begin(), value.end(), [](const unsigned char character) {
                return std::isxdigit(character) || character == '.' || character == ':';
            }))
            return {};
        return value;
    }

    static std::string required(const std::optional<ruvia::String>& value,
                                std::string_view message) {
        if (!value || value->view().empty())
            service::common::fail(15002, std::string(message), 400);
        return std::string(value->view());
    }

    static const LinkEndpointBody& requiredEndpoint(const SaveLinkBody& body) {
        if (!body.get<"endpoint">())
            service::common::fail(15002, "链路端点不能为空", 400);
        return *body.get<"endpoint">();
    }

    static const ruvia::Array<LinkTargetBody>& requiredTargets(const LinkEndpointBody& endpoint) {
        if (!endpoint.get<"targets">())
            service::common::fail(15002, "目标列表不能为空", 400);
        return *endpoint.get<"targets">();
    }

    template <typename Row>
    ruvia::Task<void> fill(ruvia::Context& c, LinkItemDto& item, const Row& row) {
        const auto id = std::string(row[0].value().value_or(std::string_view{}));
        const auto runtime = co_await loadRuntimeStatus(c, id);
        ruvia::BoxedArray<ruvia::String> clients(
            ruvia::ModelOptions{.resource = c.arena()});
        const auto clientText = runtime.text("clients");
        std::size_t start = 0;
        while (start < clientText.size()) {
            const auto end = clientText.find('\n', start);
            clients.emplace(clientText.substr(start, end == std::string::npos ? std::string::npos
                                                                              : end - start),
                            ruvia::ModelOptions{.resource = c.arena()});
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        RuntimeDto runtimeDto(ruvia::ModelOptions{.resource = c.arena()});
        runtimeDto.set<"state">(runtime.text("state", "stopped"));
        runtimeDto.set<"reason">(runtime.text("state_reason"));
        runtimeDto.set<"error">(runtime.text("error"));
        runtimeDto.set<"clientCount">(runtime.integer("connection_count"));
        runtimeDto.set<"clients">(std::move(clients));
        const auto lastActivityAt = runtime.integer("last_activity_at_ms");
        if (lastActivityAt > 0)
            runtimeDto.set<"lastActivityAt">(
                service::common::utcTimestampFromMilliseconds(lastActivityAt));
        item.set<"id">(id);
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"protocol">(row[2].value().value_or(std::string_view{}));
        item.set<"status">(row[6].value().value_or(std::string_view{}));
        item.set<"runtime">(std::move(runtimeDto));
        item.set<"createdBy">(row[7].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[8].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[9].value().value_or(std::string_view{}));
        item.set<"execution">(row[10].value().value_or("collector"));
        item.set<"edgeNodeId">(row[11].value().value_or(""));
        LinkEndpointDto endpoint(ruvia::ModelOptions{.resource = c.arena()});
        endpoint.set<"mode">(row[3].value().value_or(std::string_view{}));
        endpoint.set<"ip">(row[4].value().value_or(std::string_view{}));
        endpoint.set<"port">(toInt(row[5].value().value_or(std::string_view{})));
        endpoint.set<"targets">(co_await loadTargets(c, id, runtime));
        co_await fillEndpoint(c, endpoint, id);
        item.set<"endpoint">(std::move(endpoint));
    }

    ruvia::Task<ruvia::BoxedArray<LinkTargetDto>> loadTargets(ruvia::Context& c, std::string_view id,
                                                         const RuntimeStatus& runtime) {
        ruvia::DbQuery query(c.pool());
        const auto targets = query.coalesce(
            {jsonValue(query, "endpoint", "targets"),
             query.cast(query.value("[]"), ruvia::DbDataType::kJsonb)});
        query.select({jsonText(query, "target", "id", "value"),
                      jsonText(query, "target", "name", "value"),
                      jsonText(query, "target", "ip", "value"),
                      jsonText(query, "target", "port", "value"),
                      jsonText(query, "target", "status", "value")})
            .from(LinkEntity::tableName(), "link")
            .joinFunction(ruvia::DbJoinType::kCross,
                          query.call("jsonb_array_elements", {targets}), {}, "value",
                          {.lateral = true,
                           .withOrdinality = true,
                           .columns = {{.name = "target"}, {.name = "position"}}})
            .where(query.binary(query.column("id", "link"), ruvia::DbBinaryOperator::kEqual,
                                query.value(id)))
            .orderBy(query.column("position", "value"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<LinkTargetDto> result(
            ruvia::ModelOptions{.resource = c.arena()});
        for (const auto& row : rows) {
            auto& target = result.emplace(ruvia::ModelOptions{.resource = c.arena()});
            const auto targetId = std::string(row[0].value().value_or(std::string_view{}));
            const auto prefix = "target:" + targetId + ':';
            RuntimeDto targetRuntime(ruvia::ModelOptions{.resource = c.arena()});
            targetRuntime.set<"state">(runtime.text(prefix + "state", "stopped"))
                .set<"reason">(runtime.text(prefix + "reason"))
                .set<"error">(runtime.text(prefix + "error"));
            const auto lastActivityAt = runtime.integer(prefix + "last_activity_at_ms");
            if (lastActivityAt > 0)
                targetRuntime.set<"lastActivityAt">(
                    service::common::utcTimestampFromMilliseconds(lastActivityAt));
            target.set<"id">(targetId)
                .set<"name">(row[1].value().value_or(std::string_view{}))
                .set<"ip">(row[2].value().value_or(std::string_view{}))
                .set<"port">(toInt(row[3].value().value_or(std::string_view{})))
                .set<"status">(row[4].value().value_or(std::string_view{}))
                .set<"runtime">(std::move(targetRuntime));
        }
        co_return result;
    }

    static ruvia::Task<RuntimeStatus> loadRuntimeStatus(ruvia::Context& c, std::string_view id) {
        RuntimeStatus status;
        try {
            const auto owner = co_await c.redis().get("iot:v2:owner:link:" + std::string(id));
            if (!owner) co_return status;
            const auto pattern = "iot:runtime:link:" + std::string(id) + ":worker:" +
                                 std::string(owner->data(),owner->size()) + ":*";
            std::string cursor = "0";
            std::vector<std::string> keys;
            do {
                const std::array<std::string_view, 6> scanArgs{"SCAN", cursor, "MATCH", pattern,
                                                               "COUNT", "100"};
                const auto page = co_await c.redis().command(scanArgs);
                if (page.kind() != ruvia::RedisValue::Kind::kArray || page.array().size() != 2 ||
                    page.array()[0].kind() != ruvia::RedisValue::Kind::kString ||
                    page.array()[1].kind() != ruvia::RedisValue::Kind::kArray)
                    break;
                cursor.assign(page.array()[0].string());
                for (const auto& value : page.array()[1].array())
                    if (value.kind() == ruvia::RedisValue::Kind::kString)
                        keys.emplace_back(value.string());
            } while (cursor != "0");

            std::int64_t connectionCount = 0;
            std::int64_t lastActivityAt = 0;
            std::string clients;
            std::string aggregateState = "stopped";
            const auto stateRank = [](std::string_view state) {
                if (state == "connected")
                    return 6;
                if (state == "listening")
                    return 5;
                if (state == "reconnecting")
                    return 4;
                if (state == "connecting")
                    return 3;
                if (state == "error")
                    return 2;
                if (state == "idle")
                    return 1;
                return 0;
            };
            for (const auto& key : keys) {
                const std::array<std::string_view, 2> hashArgs{"HGETALL", key};
                const auto reply = co_await c.redis().command(hashArgs);
                if (reply.kind() != ruvia::RedisValue::Kind::kArray)
                    continue;
                RuntimeStatus worker;
                const auto values = reply.array();
                for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
                    if (values[index].kind() != ruvia::RedisValue::Kind::kString ||
                        values[index + 1].kind() != ruvia::RedisValue::Kind::kString)
                        continue;
                    worker.fields.insert_or_assign(std::string(values[index].string()),
                                                   std::string(values[index + 1].string()));
                }
                connectionCount += worker.integer("connection_count");
                lastActivityAt = std::max(lastActivityAt, worker.integer("last_activity_at_ms"));
                const auto endpoints = worker.text("remote_endpoints");
                std::size_t start = 0;
                while (start < endpoints.size()) {
                    const auto end = endpoints.find(',', start);
                    if (!clients.empty())
                        clients.push_back('\n');
                    clients.append(endpoints, start,
                                   end == std::string::npos ? std::string::npos : end - start);
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                const auto workerState = worker.text("state", "stopped");
                if (stateRank(workerState) > stateRank(aggregateState))
                    aggregateState = workerState;
                if (status.text("state_reason").empty() && !worker.text("state_reason").empty())
                    status.fields["state_reason"] = worker.text("state_reason");
                if (status.text("error").empty() && !worker.text("error").empty())
                    status.fields["error"] = worker.text("error");
                for (const auto& [name, value] : worker.fields) {
                    if (!name.starts_with("target:"))
                        continue;
                    if (!value.empty() || !status.fields.contains(name))
                        status.fields[name] = value;
                }
            }
            status.fields["state"] = aggregateState;
            status.fields["connection_count"] = std::to_string(connectionCount);
            status.fields["clients"] = std::move(clients);
            status.fields["last_activity_at_ms"] = std::to_string(lastActivityAt);
        } catch (const std::exception&) {
        }
        co_return status;
    }

    template <typename Targets>
    static void validateConfiguration(std::string_view mode, std::string_view protocol,
                                      std::string_view ip, std::int64_t port,
                                      const Targets& targets) {
        if (mode != "TCP Server" && mode != "TCP Client")
            service::common::fail(15003, "链路模式无效", 400);
        if (protocol != "SL651" && protocol != "Modbus" && protocol != "S7")
            service::common::fail(15003, "协议无效", 400);
        if (protocol == "SL651" && mode != "TCP Server")
            service::common::fail(15003, "SL651 只支持 TCP Server 模式", 400);
        if (mode == "TCP Server") {
            if (ip != "0.0.0.0")
                service::common::fail(15003, "TCP Server 监听 IP 必须是 0.0.0.0", 400);
            if (port < 1 || port > 65535)
                service::common::fail(15003, "TCP Server 必须配置有效的监听端口", 400);
            if (!targets.empty())
                service::common::fail(15003, "TCP Server 不能配置目标地址", 400);
            return;
        }
        if (!ip.empty() || port != 0)
            service::common::fail(15003, "TCP Client 不能配置监听地址", 400);
        if (targets.empty())
            service::common::fail(15003, "TCP Client 至少需要一个目标地址", 400);
        std::set<std::string> ids;
        std::set<std::string> endpoints;
        for (const auto& target : targets) {
            const auto id = required(target.template get<"id">(), "目标 ID 不能为空");
            const auto name = required(target.template get<"name">(), "目标名称不能为空");
            const auto targetIp = required(target.template get<"ip">(), "目标 IP 不能为空");
            const auto targetPort = target.template get<"port">()
                                        ? static_cast<std::int64_t>(*target.template get<"port">())
                                        : 0;
            const auto targetStatus = target.template get<"status">()
                                          ? std::string(target.template get<"status">()->view())
                                          : "enabled";
            if (targetStatus != "enabled" && targetStatus != "disabled")
                service::common::fail(15003, "目标状态无效", 400);
            if (name.empty() || !isIpv4(targetIp) || targetPort < 1 || targetPort > 65535)
                service::common::fail(15003, "目标地址配置无效", 400);
            if (!ids.emplace(id).second)
                service::common::fail(15004, "同一链路内目标 ID 不能重复", 409);
            if (!endpoints.emplace(targetIp + ":" + std::to_string(targetPort)).second)
                service::common::fail(15004, "同一链路内目标地址不能重复", 409);
        }
    }

    static bool isIpv4(std::string_view value) {
        int parts = 0;
        std::size_t start = 0;
        while (start < value.size()) {
            const auto end = value.find('.', start);
            const auto part = value.substr(
                start, end == std::string_view::npos ? value.size() - start : end - start);
            if (part.empty() || part.size() > 3)
                return false;
            int number = 0;
            for (const char ch : part) {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                    return false;
                number = number * 10 + (ch - '0');
            }
            if (number > 255)
                return false;
            ++parts;
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return parts == 4;
    }

    static void appendJsonString(std::string& output, std::string_view value) {
        static constexpr char hex[] = "0123456789abcdef";
        output.push_back('"');
        for (const unsigned char ch : value) {
            switch (ch) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[ch >> 4]);
                    output.push_back(hex[ch & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
            }
        }
        output.push_back('"');
    }

    template <typename Targets> static std::string serializeTargets(const Targets& targets) {
        std::string result = "[";
        for (const auto& target : targets) {
            if (result.size() > 1)
                result.push_back(',');
            result += "{\"id\":";
            appendJsonString(result, target.template get<"id">()->view());
            result += ",\"name\":";
            appendJsonString(result, target.template get<"name">()->view());
            result += ",\"ip\":";
            appendJsonString(result, target.template get<"ip">()->view());
            result += ",\"port\":" +
                      std::to_string(static_cast<std::int64_t>(*target.template get<"port">()));
            result += ",\"status\":";
            const auto& status = target.template get<"status">();
            appendJsonString(result, status ? status->view() : "enabled");
            result.push_back('}');
        }
        result.push_back(']');
        return result;
    }

    template <typename Targets>
    static std::string serializeEndpoint(std::string_view mode, std::string_view ip,
                                         std::int64_t port, const Targets& targets) {
        std::string result = "{\"transport\":\"tcp\",\"mode\":";
        appendJsonString(result, mode);
        result += ",\"ip\":";
        appendJsonString(result, mode == "TCP Server" ? ip : "");
        result += ",\"port\":" + std::to_string(mode == "TCP Server" ? port : 0);
        result += ",\"targets\":";
        result += mode == "TCP Client" ? serializeTargets(targets) : "[]";
        result.push_back('}');
        return result;
    }

    ruvia::Task<void> ensureAvailable(ruvia::Context& c, const std::string& name,
                                      const std::string& mode, const std::string& ip,
                                      std::int64_t port, std::optional<std::string> excludedId) {
        ruvia::DbQuery query(c.pool());
        const auto endpointPort = jsonText(query, "endpoint", "port");
        const auto endpointPortText = query.coalesce({endpointPort, query.value("")});
        const auto endpointPortIsInteger = query.binary(
            endpointPortText, ruvia::DbBinaryOperator::kRegex,
            query.value("^[0-9]{1,5}$"));
        const auto safeEndpointPort = query.caseWhen(
            {{endpointPortIsInteger, query.cast(endpointPort, ruvia::DbDataType::kInteger)}},
            query.cast(query.value(std::int64_t{0}), ruvia::DbDataType::kInteger));
        const auto nameTaken = query.binary(query.column("name"), ruvia::DbBinaryOperator::kEqual,
                                            query.value(name));
        query.select(query.cast(query.value(1), ruvia::DbDataType::kInteger))
            .from(LinkEntity::tableName())
            .where(LinkEntity::column<"deleted_at">().isNull().expression(query));
        if (mode == "TCP Server") {
            const auto serverAddress = query.binary(
                query.binary(query.column("execution"), ruvia::DbBinaryOperator::kEqual,
                             query.value("collector")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(jsonText(query, "endpoint", "mode"),
                                 ruvia::DbBinaryOperator::kEqual, query.value(mode)),
                    ruvia::DbBinaryOperator::kAnd,
                    query.binary(
                        query.binary(jsonText(query, "endpoint", "ip"),
                                     ruvia::DbBinaryOperator::kEqual, query.value(ip)),
                        ruvia::DbBinaryOperator::kAnd,
                        query.binary(safeEndpointPort, ruvia::DbBinaryOperator::kEqual,
                                     query.value(port)))));
            query.andWhere(query.binary(nameTaken, ruvia::DbBinaryOperator::kOr, serverAddress));
        } else {
            query.andWhere(nameTaken);
        }
        if (excludedId)
            query.andWhere(query.binary(query.column("id"), ruvia::DbBinaryOperator::kNotEqual,
                                        query.cast(query.value(*excludedId),
                                                   ruvia::DbDataType::kUuid)));
        query.limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty())
            service::common::fail(15005, "链路名称或监听地址已存在", 409);
    }

    ruvia::Task<void> requireOwner(ruvia::Context& c, std::string_view ownerId) {
        const auto principal = service::middleware::requireAuth(c);
        if (principal.userId == ownerId)
            co_return;
        ruvia::DbQuery query(c.pool());
        query.select(query.cast(query.value(1), ruvia::DbDataType::kInteger))
            .from(service::user::UserRoleEntity::tableName(), "ur")
            .join(ruvia::DbJoinType::kInner, service::role::RoleEntity::tableName(),
                  query.binary(query.column("role_id", "ur"), ruvia::DbBinaryOperator::kEqual,
                               query.column("id", "r")),
                  "r")
            .where(query.binary(query.column("user_id", "ur"),
                                ruvia::DbBinaryOperator::kEqual,
                                query.value(principal.userId)))
            .andWhere(query.binary(query.column("code", "r"),
                                   ruvia::DbBinaryOperator::kEqual,
                                   query.value("superadmin")))
            .andWhere(query.binary(query.column("status", "r"),
                                   ruvia::DbBinaryOperator::kEqual,
                                   query.value("enabled")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull,
                                  query.column("deleted_at", "r")))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty())
            service::common::fail(15007, "只能修改或删除自己创建的链路", 403);
    }

    OutboundHttp http_;
    std::mutex publicIpMutex_;
    std::string cachedPublicIp_;
    std::chrono::steady_clock::time_point publicIpCachedAt_{};
};

inline LinkService& linkService() { return LinkService::instance(); }

} // namespace service::link
