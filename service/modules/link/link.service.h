#pragma once
#include <ruvia/web/Validation.h>
#include <system_error>
#include <asio/ip/address_v4.hpp>

#include <memory>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/HttpClientHandle.h>
#include <ruvia/web/HttpClientResponse.h>
#include <ruvia/web/ModelJson.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/timestamp.h"
#include "service/common/uuid.h"
#include "service/modules/edge_node/edge_node.service.h"
#include "service/modules/link/link.entity.h"
#include "service/modules/link/link.types.h"
#include "service/modules/system/outbox/outbox.service.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/user/user.entity.h"
#include "service/utils/number.h"
#include "service/utils/redis.h"

namespace service::link {

class LinkService {
  public:
    static LinkService& instance() {
        static thread_local LinkService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<LinkPageDataDto> list(Context& c, std::int64_t page, std::int64_t pageSize, std::optional<std::string> keyword, std::optional<std::string> mode, std::optional<std::string> protocol, std::optional<std::string> status) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 100);
        ruvia::DbQuery count(c.pool());
        count.select(count.aggregate("count", { count.star() })).from(LinkEntity::tableName());
        applyFilters(count, keyword, mode, protocol, status);
        const auto countRows = co_await c.db().query(count);
        const ruvia::Int64 total = countRows.empty()
            ? ruvia::Int64{ 0 }
            : toInt(countRows.front()[0].value().value_or(std::string_view{}));

        auto query = linkSelect(c.pool());
        applyFilters(query, keyword, mode, protocol, status);
        query.orderBy(query.column("id"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>((page - 1) * pageSize));
        const auto rows = co_await c.db().query(query);

        ruvia::BoxedArray<LinkItemDto> links(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : rows) {
            auto& item = links.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            co_await fill(c, item, row);
        }
        LinkPageDataDto result(ruvia::ModelOptions{ .resource = c.arena() });
        result.set<"list">(std::move(links))
            .template set<"total">(total)
            .template set<"page">(page)
            .template set<"pageSize">(pageSize)
            .template set<"totalPages">(total == 0 ? 0 : (total + pageSize - 1) / pageSize);
        co_return result;
    }

    template <typename Context>
    ruvia::Task<LinkItemDto> detail(Context& c, std::string_view id) {
        auto query = linkSelect(c.pool());
        query.where((LinkEntity::column<"id">() == id &&
                     LinkEntity::column<"deleted_at">().isNull())
                        .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(15001, "链路不存在", 404);
        }
        LinkItemDto item(ruvia::ModelOptions{ .resource = c.arena() });
        co_await fill(c, item, rows.front());
        co_return item;
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<LinkOptionDto>> options(Context& c) {
        auto query = linkOptionSelect(c.pool());
        query.where((LinkEntity::column<"deleted_at">().isNull() &&
                     LinkEntity::column<"status">() == "enabled")
                        .expression(query))
            .orderBy(query.column("name"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<LinkOptionDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& item = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            LinkEndpointDto endpoint(ruvia::ModelOptions{ .resource = c.arena() });
            endpoint.set<"mode">(row[3].value().value_or(std::string_view{}))
                .template set<"ip">(row[4].value().value_or(std::string_view{}))
                .template set<"port">(toInt(row[5].value().value_or(std::string_view{})))
                .template set<"targets">(co_await loadTargets(c, row[0].value().value_or(std::string_view{}), RuntimeStatus{}));
            item.set<"execution">(row[6].value().value_or("collector"));
            item.set<"edgeNodeId">(row[7].value().value_or(""));
            co_await fillEndpoint(c, endpoint, row[0].value().value_or(""));
            item.set<"id">(row[0].value().value_or(std::string_view{}))
                .template set<"name">(row[1].value().value_or(std::string_view{}))
                .template set<"protocol">(row[2].value().value_or(std::string_view{}))
                .template set<"endpoint">(std::move(endpoint));
        }
        co_return result;
    }

    template <typename Context>
    LinkEnumsDto enums(Context& c) {
        LinkEnumsDto result(ruvia::ModelOptions{ .resource = c.arena() });
        const auto modelOptions = ruvia::ModelOptions{ .resource = c.arena() };
        ruvia::BoxedArray<ruvia::String> modes(modelOptions);
        modes.emplace("TCP Server", modelOptions);
        modes.emplace("TCP Client", modelOptions);
        ruvia::BoxedArray<ruvia::String> protocols(modelOptions);
        protocols.emplace("SL651", modelOptions);
        protocols.emplace("Modbus", modelOptions);
        protocols.emplace("S7", modelOptions);
        protocols.emplace("MC", modelOptions);
        protocols.emplace("FINS", modelOptions);
        protocols.emplace("DLT645", modelOptions);
        ruvia::BoxedArray<ruvia::String> statuses(modelOptions);
        statuses.emplace("enabled", modelOptions);
        statuses.emplace("disabled", modelOptions);
        result.set<"modes">(std::move(modes))
            .template set<"protocols">(std::move(protocols))
            .template set<"statuses">(std::move(statuses));
        return result;
    }

    // 客户端、请求和缓存均由接入请求的 Service Worker 持有。
    template <typename Context>
    ruvia::Task<std::string> publicIp(Context& c) {
        const auto now = std::chrono::steady_clock::now();
        if (!cachedPublicIp_.empty() && now - publicIpCachedAt_ < std::chrono::minutes(5)) {
            co_return cachedPublicIp_;
        }
        try {
            const std::array<ruvia::HttpHeaderView, 1> headers{ { { "Accept", "text/plain" } } };
            auto response = co_await c.httpClient("link-public-ip").send({ .headers = headers });
            if (response.status().value() == 200) {
                const auto body = co_await response.body().readAll(64U * 1024U);
                const auto resolved = parsePublicIp(body);
                if (!resolved.empty()) {
                    cachedPublicIp_ = resolved;
                    publicIpCachedAt_ = std::chrono::steady_clock::now();
                }
            }
        } catch (const ruvia::HttpClientError&) {
            // 查询失败保留旧值；首次查询失败仍返回空字符串。
        }
        co_return cachedPublicIp_;
    }

    template <typename Context>
    ruvia::Task<void> create(Context& c, const SaveLinkBody& body) {
        if (body.get<"execution">() && body.get<"execution">()->view() == "edge") {
            co_await saveEdgeChannel(c, {}, body);
            co_return;
        }
        const auto name = std::string(body.get<"name">().view());
        const auto protocol = std::string(body.get<"protocol">().view());
        const auto& endpoint = body.get<"endpoint">();
        const auto mode = required(endpoint.get<"mode">(), "链路模式不能为空");
        const auto ip = endpoint.get<"ip">() ? std::string(endpoint.get<"ip">()->view()) : "";
        const auto port = endpoint.get<"port">() ? static_cast<std::int64_t>(*endpoint.get<"port">()) : 0;
        const auto status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        validateStatus(status);
        const auto& targets = requiredTargets(endpoint);
        validateConfiguration(mode, protocol, ip, port, targets);
        co_await ensureAvailable(c, name, mode, ip, port, std::nullopt);
        const auto endpointJson = serializeEndpoint(mode, ip, port, targets);
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        auto transaction = co_await c.db().beginTransaction();
        LinkEntity link(c.pool());
        link.set<"id">(id);
        link.set<"name">(name);
        link.set<"protocol">(protocol);
        link.set<"endpoint">(endpointJson);
        link.set<"status">(status);
        link.set<"created_by">(c.userId);
        link.set<"execution">("collector");
        (void)co_await transaction.template getRepository<LinkEntity>().insert(link);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "created", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<LinkDebugAcquisitionDto>> debugPackets(Context& c, std::string_view id) {
        (void)co_await detail(c, id);
        const auto key = service::link::LinkDebugIndex::key(id);
        static constexpr std::string_view readPackets = R"lua(
local result={}
local count=0
for _,acquisition in ipairs(redis.call('ZREVRANGE',KEYS[1],0,99)) do
    local round='iot:debug:v4:acquisition:'..acquisition
    local metadata=redis.call('HGETALL',round)
    if #metadata==0 then
        redis.call('ZREM',KEYS[1],acquisition)
    else
        local ids=redis.call('ZRANGE',round..':packets',0,-1)
        if count+#ids>4096 then break end
        local packets={}
        for _,id in ipairs(ids) do
            local fields=redis.call('HGETALL','iot:debug:v4:packet:'..id)
            if #fields>0 then packets[#packets+1]={id,fields} end
        end
        result[#result+1]={acquisition,metadata,packets}
        count=count+#packets
    end
end
return result
)lua";
        const std::vector<std::string_view> keys{ key };
        const std::vector<std::string_view> args;
        const auto reply = co_await c.redis().eval(readPackets, keys, args);
        if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
            service::message::redis::throwValue("read debug packets", reply);
        }
        ruvia::BoxedArray<LinkDebugAcquisitionDto> result(ruvia::ModelOptions{ .resource = c.arena() });
        for (const auto& row : reply.array()) {
            if (row.kind() != ruvia::RedisValue::Kind::kArray || row.array().size() != 3) {
                continue;
            }
            auto& acquisition = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            acquisition.set<"id">(row.array()[0].string());
            const auto metadata = row.array()[1].array();
            for (std::size_t index = 0; index + 1 < metadata.size(); index += 2) {
                const auto field = metadata[index].string();
                const auto value = metadata[index + 1].string();
                if (field == "started_at_ms") {
                    acquisition.set<"startedAtMs">(value);
                }
                if (field == "finished_at_ms") {
                    acquisition.set<"finishedAtMs">(value);
                }
                if (field == "last_packet_at_ms") {
                    acquisition.set<"lastPacketAtMs">(value);
                }
                if (field == "state") {
                    acquisition.set<"state">(value);
                }
                if (field == "device_id") {
                    acquisition.set<"deviceId">(value);
                }
            }
            ruvia::BoxedArray<LinkDebugPacketDto> packets(ruvia::ModelOptions{ .resource = c.arena() });
            for (const auto& packetRow : row.array()[2].array()) {
                const auto fields = packetRow.array()[1].array();
                std::string eventId(packetRow.array()[0].string());
                auto& packet = packets.emplace(ruvia::ModelOptions{ .resource = c.arena() });
                packet.set<"id">(eventId);
                for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
                    const auto name = fields[index].string();
                    const auto value = fields[index + 1].string();
                    if (name == "acquisition_id") {
                        packet.set<"acquisitionId">(value);
                    } else if (name == "device_id") {
                        packet.set<"deviceId">(value);
                    } else if (name == "direction") {
                        packet.set<"direction">(value);
                    } else if (name == "source") {
                        packet.set<"source">(value);
                    } else if (name == "address") {
                        packet.set<"address">(value);
                    } else if (name == "edge_node_id") {
                        packet.set<"edgeNodeId">(value);
                    } else if (name == "edge_node_name") {
                        packet.set<"edgeNodeName">(value);
                    } else if (name == "payload_hex") {
                        packet.set<"payloadHex">(value);
                    } else if (name == "time_ms") {
                        packet.set<"timeMs">(value);
                    } else if (name == "transport_status") {
                        packet.set<"transportStatus">(value);
                    } else if (name == "response_status") {
                        packet.set<"responseStatus">(value);
                    } else if (name == "parse_status") {
                        packet.set<"parseStatus">(value);
                    } else if (name == "revision") {
                        packet.set<"revision">(value);
                    } else if (name == "reply_to_packet_id") {
                        packet.set<"replyToPacketId">(value);
                    } else if (name == "reason") {
                        packet.set<"reason">(value);
                    } else if (name == "parsed_json") {
                        packet.set<"parsedJson">(value);
                    }
                }
            }
            acquisition.set<"packets">(std::move(packets));
        }
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> setDebug(Context& c, std::string_view id, bool enabled) {
        (void)co_await detail(c, id);
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbQuery query(c.pool());
        query.update(LinkEntity::tableName())
            .set(LinkEntity::columnName<"debug_enabled">(), query.value(enabled))
            .set("updated_at", query.call("now"))
            .where((LinkEntity::column<"id">() == id && LinkEntity::column<"deleted_at">().isNull()).expression(query));
        (void)co_await transaction.execute(query);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        const auto link = co_await detail(c, id);
        if (link.template get<"edgeNodeId">() && !link.template get<"edgeNodeId">()->view().empty()) {
            (void)co_await service::edge::EdgeService::queueSnapshot(c, link.template get<"edgeNodeId">()->view(), c.userId);
        }
    }

    template <typename Context>
    ruvia::Task<void> update(Context& c, std::string_view id, const SaveLinkBody& body) {
        if (body.get<"execution">() && body.get<"execution">()->view() == "edge") {
            co_await saveEdgeChannel(c, id, body);
            co_return;
        }
        ruvia::DbQuery lookup(c.pool());
        lookup
            .select({ jsonText(lookup, "endpoint", "mode"), lookup.column("protocol"), lookup.column("created_by") })
            .from(LinkEntity::tableName())
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"deleted_at">().isNull() &&
                    LinkEntity::column<"execution">() == "collector")
                       .expression(lookup))
            .limit(1);
        const auto rows = co_await c.db().query(lookup);
        if (rows.empty()) {
            service::common::fail(15001, "链路不存在", 404);
        }
        co_await requireOwner(c, rows.front()[2].value().value_or(std::string_view{}));

        const auto name = std::string(body.get<"name">().view());
        const auto protocol = std::string(body.get<"protocol">().view());
        const auto& endpoint = body.get<"endpoint">();
        const auto mode = required(endpoint.get<"mode">(), "链路模式不能为空");
        if (mode != rows.front()[0].value().value_or(std::string_view{}) || protocol != rows.front()[1].value().value_or(std::string_view{})) {
            service::common::fail(15006, "链路模式和协议创建后不能修改", 400);
        }
        const auto ip = endpoint.get<"ip">() ? std::string(endpoint.get<"ip">()->view()) : "";
        const auto port = endpoint.get<"port">() ? static_cast<std::int64_t>(*endpoint.get<"port">()) : 0;
        const auto status = body.get<"status">() ? std::string(body.get<"status">()->view()) : "enabled";
        validateStatus(status);
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
        const auto nameChanged = query.binary(query.column("name"), ruvia::DbBinaryOperator::kIsDistinctFrom, query.value(name));
        const auto endpointChanged = query.binary(
            query.column("endpoint"),
            ruvia::DbBinaryOperator::kIsDistinctFrom,
            query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb)
        );
        const auto statusChanged = query.binary(query.column("status"), ruvia::DbBinaryOperator::kIsDistinctFrom, query.value(status));
        query.andWhere(query.binary(query.binary(nameChanged, ruvia::DbBinaryOperator::kOr, endpointChanged), ruvia::DbBinaryOperator::kOr, statusChanged));
        const auto updated = co_await transaction.execute(query);
        if (updated.affectedRows() != 0) {
            co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        }
        co_await transaction.commit();
    }

    template <typename Context>
    ruvia::Task<void> remove(Context& c, std::string_view id) {
        ruvia::DbFindOptions options;
        options.where = LinkEntity::column<"id">() == id && LinkEntity::column<"deleted_at">().isNull();
        const auto link = co_await c.db().template getRepository<LinkEntity>().findOne(options);
        if (!link) {
            service::common::fail(15001, "链路不存在", 404);
        }
        co_await requireOwner(c, link->template get<"created_by">());
        ruvia::DbFindOptions used;
        used.where = entities::DeviceEntity::column<"link_id">() == id &&
            entities::DeviceEntity::column<"deleted_at">().isNull();
        if (co_await c.db().template getRepository<entities::DeviceEntity>().exists(used)) {
            service::common::fail(15008, "链路已被设备使用，请先删除关联设备", 409);
        }
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbExpressions expressions(c.pool());
        (void)co_await transaction.template getRepository<LinkEntity>().update(
            LinkEntity::column<"id">() == id && LinkEntity::column<"deleted_at">().isNull(),
            { { "deleted_at", expressions.call("now") }, { "updated_at", expressions.call("now") } }
        );
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "link", "deleted", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
    }

  private:

    static std::string required(const std::optional<ruvia::String>& value, std::string_view message) {
        if (!value || value->view().empty()) {
            service::common::fail(15002, std::string(message), 400);
        }
        return std::string(value->view());
    }



    static const ruvia::Array<LinkTargetBody>& requiredTargets(const LinkEndpointBody& endpoint) {
        if (!endpoint.get<"targets">()) {
            service::common::fail(15002, "目标列表不能为空", 400);
        }
        return *endpoint.get<"targets">();
    }

    static void validateStatus(std::string_view status) {
        if (status != "enabled" && status != "disabled") {
            service::common::fail(15002, "状态无效", 400);
        }
    }

    static void validateNodeId(std::string_view nodeId) {
        if (!service::common::isUuid(nodeId)) {
            service::common::fail(15002, "节点 ID 无效", 400);
        }
    }

    static void validateEdgeEndpoint(const LinkEndpointBody& endpoint, std::string_view protocol, std::string_view transport, std::string_view interfaceName) {
        if (interfaceName.size() > 96) {
            service::common::fail(15002, "接口名称过长", 400);
        }
        if (transport == "serial") {
            if (protocol == "S7" || protocol == "MC" || protocol == "FINS") {
                service::common::fail(15002, "所选 PLC 协议仅支持 TCP", 400);
            }
            const auto baud = endpoint.get<"baudRate">().value_or(9600);
            const auto bits = endpoint.get<"dataBits">().value_or(8);
            const auto stops = endpoint.get<"stopBits">().value_or(1);
            const auto parity = endpoint.get<"parity">() ? endpoint.get<"parity">()->view() : std::string_view("none");
            if (baud < 300 || baud > 4000000 || bits < 5 || bits > 8 || stops < 1 || stops > 2 ||
                (parity != "none" && parity != "odd" && parity != "even")) {
                service::common::fail(15002, "串口参数无效", 400);
            }
        } else if (transport == "tcp") {
            const auto mode = required(endpoint.get<"mode">(), "请选择 TCP 模式");
            const auto ip = required(endpoint.get<"ip">(), "请输入 IP 地址");
            std::error_code error;
            (void)asio::ip::make_address_v4(ip, error);
            const auto port = endpoint.get<"port">().value_or(0);
            if (error || port < 1 || port > 65535 || (mode != "TCP Client" && mode != "TCP Server")) {
                service::common::fail(15002, "TCP 参数无效", 400);
            }
        } else {
            service::common::fail(15002, "传输类型无效", 400);
        }
    }

    template <typename Targets>
    static void validateConfiguration(std::string_view mode, std::string_view protocol, std::string_view ip, std::int64_t port, const Targets& targets) {
        if (mode != "TCP Server" && mode != "TCP Client") {
            service::common::fail(15003, "链路模式无效", 400);
        }
        if (protocol != "SL651" && protocol != "Modbus" && protocol != "S7" && protocol != "MC" && protocol != "FINS" && protocol != "DLT645") {
            service::common::fail(15003, "协议无效", 400);
        }
        if (protocol == "SL651" && mode != "TCP Server") {
            service::common::fail(15003, "SL651 只支持 TCP Server 模式", 400);
        }
        if (mode == "TCP Server") {
            if (ip != "0.0.0.0") {
                service::common::fail(15003, "TCP Server 监听 IP 必须是 0.0.0.0", 400);
            }
            if (port < 1 || port > 65535) {
                service::common::fail(15003, "TCP Server 必须配置有效的监听端口", 400);
            }
            if (!targets.empty()) {
                service::common::fail(15003, "TCP Server 不能配置目标地址", 400);
            }
            return;
        }
        if (!ip.empty() || port != 0) {
            service::common::fail(15003, "TCP Client 不能配置监听地址", 400);
        }
        if (targets.empty()) {
            service::common::fail(15003, "TCP Client 至少需要一个目标地址", 400);
        }
        std::set<std::string> ids;
        std::set<std::string> endpoints;
        for (const auto& target : targets) {
            const auto id = std::string(target.template get<"id">().view());
            const auto name = std::string(target.template get<"name">().view());
            const auto targetIp = std::string(target.template get<"ip">().view());
            const auto targetPort = static_cast<std::int64_t>(target.template get<"port">());
            const auto targetStatus = target.template get<"status">()
                ? std::string(target.template get<"status">()->view())
                : "enabled";
            if (targetStatus != "enabled" && targetStatus != "disabled") {
                service::common::fail(15003, "目标状态无效", 400);
            }
            if (name.empty() || !isIpv4(targetIp) || targetPort < 1 || targetPort > 65535) {
                service::common::fail(15003, "目标地址配置无效", 400);
            }
            if (!ids.emplace(id).second) {
                service::common::fail(15004, "同一链路内目标 ID 不能重复", 409);
            }
            if (!endpoints.emplace(targetIp + ":" + std::to_string(targetPort)).second) {
                service::common::fail(15004, "同一链路内目标地址不能重复", 409);
            }
        }
    }

    static bool isIpv4(std::string_view value) {
        int parts = 0;
        std::size_t start = 0;
        while (start < value.size()) {
            const auto end = value.find('.', start);
            const auto part = value.substr(
                start,
                end == std::string_view::npos ? value.size() - start : end - start
            );
            if (part.empty() || part.size() > 3) {
                return false;
            }
            int number = 0;
            for (const char ch : part) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) {
                    return false;
                }
                number = number * 10 + (ch - '0');
            }
            if (number > 255) {
                return false;
            }
            ++parts;
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        return parts == 4;
    }

    static ruvia::DbExpression jsonText(ruvia::DbQuery& query, std::string_view column, std::string_view key, std::string_view table = {}) {
        return query.binary(query.column(column, table), ruvia::DbBinaryOperator::kJsonGetText, query.cast(query.value(key), ruvia::DbDataType::kText));
    }

    static ruvia::DbExpression jsonValue(ruvia::DbQuery& query, std::string_view column, std::string_view key, std::string_view table = {}) {
        return query.binary(query.column(column, table), ruvia::DbBinaryOperator::kJsonGet, query.cast(query.value(key), ruvia::DbDataType::kText));
    }

    static ruvia::DbQuery linkSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto endpointPort = query.coalesce(
            { query.nullIf(jsonText(query, "endpoint", "port"), query.value("")),
              query.value("0") }
        );
        query.select({ query.cast(query.column("id"), ruvia::DbDataType::kText), query.column("name"), query.column("protocol"), jsonText(query, "endpoint", "mode"), query.coalesce({ jsonText(query, "endpoint", "ip"), query.value("") }), endpointPort, query.column("status"), query.cast(query.column("created_by"), ruvia::DbDataType::kText), query.call("iot_utc_timestamp", { query.column("created_at") }), query.call("iot_utc_timestamp", { query.column("updated_at") }), query.column("execution"), query.coalesce({ query.cast(query.column("edge_node_id"), ruvia::DbDataType::kText), query.value("") }), query.column(LinkEntity::columnName<"debug_enabled">()) })
            .from(LinkEntity::tableName());
        return query;
    }

    static ruvia::DbQuery linkOptionSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        const auto endpointPort = query.coalesce(
            { query.nullIf(jsonText(query, "endpoint", "port"), query.value("")),
              query.value("0") }
        );
        query.select({ query.cast(query.column("id"), ruvia::DbDataType::kText), query.column("name"), query.column("protocol"), jsonText(query, "endpoint", "mode"), query.coalesce({ jsonText(query, "endpoint", "ip"), query.value("") }), endpointPort, query.column("execution"), query.coalesce({ query.cast(query.column("edge_node_id"), ruvia::DbDataType::kText), query.value("") }) })
            .from(LinkEntity::tableName());
        return query;
    }

    static void applyFilters(ruvia::DbQuery& query, const std::optional<std::string>& keyword, const std::optional<std::string>& mode, const std::optional<std::string>& protocol, const std::optional<std::string>& status) {
        query.where(LinkEntity::column<"deleted_at">().isNull().expression(query));
        if (keyword && !keyword->empty()) {
            query.andWhere(LinkEntity::column<"name">().ilike("%" + *keyword + "%").expression(query));
        }
        if (mode && !mode->empty()) {
            query.andWhere(query.binary(jsonText(query, "endpoint", "mode"), ruvia::DbBinaryOperator::kEqual, query.value(*mode)));
        }
        if (protocol && !protocol->empty()) {
            query.andWhere((LinkEntity::column<"protocol">() == *protocol).expression(query));
        }
        if (status && !status->empty()) {
            query.andWhere((LinkEntity::column<"status">() == *status).expression(query));
        }
    }

    template <typename Context>
    static ruvia::Task<void> fillEndpoint(Context& c, LinkEndpointDto& endpoint, std::string_view id) {
        ruvia::DbQuery query(c.pool());
        query.select({ jsonText(query, "endpoint", "transport"), jsonText(query, "endpoint", "interface"), jsonText(query, "endpoint", "baud_rate"), jsonText(query, "endpoint", "data_bits"), jsonText(query, "endpoint", "stop_bits"), jsonText(query, "endpoint", "parity"), jsonText(query, "endpoint", "rs485") })
            .from(LinkEntity::tableName())
            .where((LinkEntity::column<"id">() == id &&
                    LinkEntity::column<"execution">() == "edge")
                       .expression(query))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            co_return;
        }
        const auto& row = rows.front();
        endpoint.set<"transport">(row[0].value().value_or(""));
        endpoint.set<"interfaceName">(row[1].value().value_or(""));
        endpoint.set<"baudRate">(toInt(row[2].value().value_or("9600")));
        endpoint.set<"dataBits">(toInt(row[3].value().value_or("8")));
        endpoint.set<"stopBits">(toInt(row[4].value().value_or("1")));
        endpoint.set<"parity">(row[5].value().value_or("none"));
        endpoint.set<"rs485">(row[6].value().value_or("false") == "true");
    }

    template <typename Context>
    ruvia::Task<void> saveEdgeChannel(Context& c, std::string_view existingId, const SaveLinkBody& body) {
        const auto name = std::string(body.get<"name">().view());
        const auto protocol = std::string(body.get<"protocol">().view());
        const auto nodeId = required(body.get<"edgeNodeId">(), "请选择边缘节点");
        validateNodeId(nodeId);
        const auto& endpoint = body.get<"endpoint">();
        const auto transport = required(endpoint.get<"transport">(), "请选择传输类型");
        const auto interfaceName = required(endpoint.get<"interfaceName">(), "请选择接口");
        validateEdgeEndpoint(endpoint, protocol, transport, interfaceName);
        ruvia::DbQuery nodeQuery(c.pool());
        nodeQuery.select(nodeQuery.cast(nodeQuery.value(1), ruvia::DbDataType::kInteger))
            .from(service::link::entities::EdgeNodeEntity::tableName())
            .where(nodeQuery.binary(nodeQuery.column(service::link::entities::EdgeNodeEntity::columnName<"id">()), ruvia::DbBinaryOperator::kEqual, nodeQuery.cast(nodeQuery.value(nodeId), ruvia::DbDataType::kUuid)))
            .andWhere(nodeQuery.binary(nodeQuery.column(service::link::entities::EdgeNodeEntity::columnName<"enrollment_status">()), ruvia::DbBinaryOperator::kEqual, nodeQuery.value("approved")))
            .andWhere(nodeQuery.binary(jsonText(nodeQuery, "capability", "deviceConfig"), ruvia::DbBinaryOperator::kEqual, nodeQuery.value("true")));
        if (protocol == "MC" || protocol == "FINS" || protocol == "DLT645") {
            nodeQuery.andWhere(nodeQuery.call("jsonb_exists", { nodeQuery.binary(nodeQuery.column(service::link::entities::EdgeNodeEntity::columnName<"capability">()), ruvia::DbBinaryOperator::kJsonGet, nodeQuery.value("protocols")), nodeQuery.value(protocol) }));
        }
        const auto node = co_await c.db().query(nodeQuery);
        if (node.empty()) {
            service::common::fail(15002, "节点未批准或不支持采集配置", 400);
        }
        if (transport == "serial") {
            ruvia::DbQuery serialQuery(c.pool());
            serialQuery.select(serialQuery.cast(serialQuery.value(1), ruvia::DbDataType::kInteger))
                .from(service::link::entities::EdgeNodeSerialEntity::tableName())
                .where(serialQuery.binary(serialQuery.column(service::link::entities::EdgeNodeSerialEntity::columnName<"node_id">()), ruvia::DbBinaryOperator::kEqual, serialQuery.cast(serialQuery.value(nodeId), ruvia::DbDataType::kUuid)))
                .andWhere(serialQuery.binary(serialQuery.column(service::link::entities::EdgeNodeSerialEntity::columnName<"path">()), ruvia::DbBinaryOperator::kEqual, serialQuery.value(interfaceName)))
                .andWhere(serialQuery.unary(ruvia::DbUnaryOperator::kIsTrue, serialQuery.column(service::link::entities::EdgeNodeSerialEntity::columnName<"available">())));
            const auto serial = co_await c.db().query(serialQuery);
            if (serial.empty()) {
                service::common::fail(15002, "所选串口不存在或当前不可用", 409);
            }
        } else {
            ruvia::DbQuery networkQuery(c.pool());
            networkQuery.select(networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"ipv4">()))
                .from(service::link::entities::EdgeNodeInterfaceEntity::tableName())
                .where(networkQuery.binary(networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"node_id">()), ruvia::DbBinaryOperator::kEqual, networkQuery.cast(networkQuery.value(nodeId), ruvia::DbDataType::kUuid)))
                .andWhere(networkQuery.binary(networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"name">()), ruvia::DbBinaryOperator::kEqual, networkQuery.value(interfaceName)))
                .andWhere(networkQuery.binary(
                    networkQuery.coalesce({ networkQuery.column(service::link::entities::EdgeNodeInterfaceEntity::columnName<"ipv4">()), networkQuery.value("") }),
                    ruvia::DbBinaryOperator::kNotEqual,
                    networkQuery.value("")
                ))
                .limit(1);
            const auto network = co_await c.db().query(networkQuery);
            if (network.empty()) {
                service::common::fail(15002, "所选网口不存在或未上报 IPv4", 409);
            }
            const auto mode = endpoint.get<"mode">()->view();
            const auto ip = endpoint.get<"ip">()->view();
            if ((protocol == "S7" && mode != "TCP Client") || (protocol == "SL651" && mode != "TCP Server")) {
                service::common::fail(15002, "协议不支持所选 TCP 模式", 400);
            }
            if (mode == "TCP Server" && ip != "0.0.0.0" && ip != network.front()[0].value().value_or("")) {
                service::common::fail(15002, "监听地址必须是所选网口地址", 400);
            }
        }
        std::string priorNode;
        if (!existingId.empty()) {
            ruvia::DbQuery currentQuery(c.pool());
            currentQuery.select({ currentQuery.column("created_by"), currentQuery.cast(currentQuery.column("edge_node_id"), ruvia::DbDataType::kText) })
                .from(LinkEntity::tableName())
                .where((LinkEntity::column<"id">() == existingId &&
                        LinkEntity::column<"execution">() == "edge" &&
                        LinkEntity::column<"deleted_at">().isNull())
                           .expression(currentQuery))
                .limit(1);
            const auto current = co_await c.db().query(currentQuery);
            if (current.empty()) {
                service::common::fail(15001, "通道不存在", 404);
            }
            co_await requireOwner(c, current.front()[0].value().value_or(""));
            priorNode = current.front()[1].value().value_or("");
        }
        const auto id = existingId.empty() ? c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next() : std::string(existingId);
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
            endpointJson += ",\"mode\":";
            appendJsonString(endpointJson, endpoint.get<"mode">()->view());
            endpointJson += ",\"ip\":";
            appendJsonString(endpointJson, endpoint.get<"ip">()->view());
            endpointJson += ",\"port\":" + std::to_string(*endpoint.get<"port">());
        }
        endpointJson += '}';
        const auto status = body.get<"status">() ? body.get<"status">()->view() : std::string_view("enabled");
        auto tx = co_await c.db().beginTransaction();
        if (existingId.empty()) {
            ruvia::DbQuery query(c.pool());
            query.insertInto(LinkEntity::tableName(), { "id", "name", "protocol", "endpoint", "status", "created_by", "execution", "edge_node_id" })
                .values({ query.cast(query.value(id), ruvia::DbDataType::kUuid), query.value(name), query.value(protocol), query.cast(query.value(endpointJson), ruvia::DbDataType::kJsonb), query.value(status), query.cast(query.value(c.userId), ruvia::DbDataType::kUuid), query.value("edge"), query.cast(query.value(nodeId), ruvia::DbDataType::kUuid) });
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
        co_await service::system::OutboxService::enqueueConfigEvent(tx, "link", existingId.empty() ? "created" : "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await tx.commit();
        (void)co_await service::edge::EdgeService::queueSnapshot(c, nodeId, c.userId);
        if (!priorNode.empty() && priorNode != nodeId) {
            (void)co_await service::edge::EdgeService::queueSnapshot(c, priorNode, c.userId);
        }
    }

    struct RuntimeStatus {
        std::map<std::string, std::string> fields;

        [[nodiscard]] std::string text(std::string_view name, std::string_view fallback = {}) const {
            const auto current = fields.find(std::string(name));
            return current == fields.end() ? std::string(fallback) : current->second;
        }

        [[nodiscard]] std::int64_t integer(std::string_view name) const {
            const auto value = text(name);
            return service::utils::parseInt64(std::optional<std::string_view>{ value })
                .value_or(0);
        }
    };

    static ruvia::Int64 toInt(std::string_view value) {
        return static_cast<ruvia::Int64>(
            service::utils::parseInt64(std::optional<std::string_view>{ value }).value_or(0)
        );
    }

    static std::string parsePublicIp(std::string_view body) {
        std::string value(body);
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
        if (value.size() > 64 ||
            !std::all_of(value.begin(), value.end(), [](const unsigned char character) {
                return std::isxdigit(character) || character == '.' || character == ':';
            })) {
            return {};
        }
        return value;
    }

    template <typename Context, typename Row>
    ruvia::Task<void> fill(Context& c, LinkItemDto& item, const Row& row) {
        const auto id = std::string(row[0].value().value_or(std::string_view{}));
        const auto runtime = co_await loadRuntimeStatus(c, id);
        ruvia::BoxedArray<ruvia::String> clients(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        const auto clientText = runtime.text("clients");
        std::size_t start = 0;
        while (start < clientText.size()) {
            const auto end = clientText.find('\n', start);
            clients.emplace(clientText.substr(start, end == std::string::npos ? std::string::npos : end - start), ruvia::ModelOptions{ .resource = c.arena() });
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        RuntimeDto runtimeDto(ruvia::ModelOptions{ .resource = c.arena() });
        runtimeDto.set<"state">(runtime.text("state", "stopped"));
        runtimeDto.set<"reason">(runtime.text("state_reason"));
        runtimeDto.set<"error">(runtime.text("error"));
        runtimeDto.set<"clientCount">(runtime.integer("connection_count"));
        runtimeDto.set<"clients">(std::move(clients));
        const auto lastActivityAt = runtime.integer("last_activity_at_ms");
        if (lastActivityAt > 0) {
            runtimeDto.set<"lastActivityAt">(
                service::common::utcTimestampFromMilliseconds(lastActivityAt)
            );
        }
        item.set<"id">(id);
        item.set<"name">(row[1].value().value_or(std::string_view{}));
        item.set<"protocol">(row[2].value().value_or(std::string_view{}));
        item.set<"status">(row[6].value().value_or(std::string_view{}));
        item.set<"runtime">(std::move(runtimeDto));
        item.set<"createdBy">(row[7].value().value_or(std::string_view{}));
        item.set<"createdAt">(row[8].value().value_or(std::string_view{}));
        item.set<"updatedAt">(row[9].value().value_or(std::string_view{}));
        item.set<"debugEnabled">(row[12].value().value_or("") == "t");
        item.set<"execution">(row[10].value().value_or("collector"));
        item.set<"edgeNodeId">(row[11].value().value_or(""));
        LinkEndpointDto endpoint(ruvia::ModelOptions{ .resource = c.arena() });
        endpoint.set<"mode">(row[3].value().value_or(std::string_view{}));
        endpoint.set<"ip">(row[4].value().value_or(std::string_view{}));
        endpoint.set<"port">(toInt(row[5].value().value_or(std::string_view{})));
        endpoint.set<"targets">(co_await loadTargets(c, id, runtime));
        co_await fillEndpoint(c, endpoint, id);
        item.set<"endpoint">(std::move(endpoint));
    }

    template <typename Context>
    ruvia::Task<ruvia::BoxedArray<LinkTargetDto>> loadTargets(Context& c, std::string_view id, const RuntimeStatus& runtime) {
        ruvia::DbQuery query(c.pool());
        const auto targets = query.coalesce(
            { jsonValue(query, "endpoint", "targets"),
              query.cast(query.value("[]"), ruvia::DbDataType::kJsonb) }
        );
        query.select({ jsonText(query, "target", "id", "value"), jsonText(query, "target", "name", "value"), jsonText(query, "target", "ip", "value"), jsonText(query, "target", "port", "value"), jsonText(query, "target", "status", "value") })
            .from(LinkEntity::tableName(), "link")
            .joinFunction(ruvia::DbJoinType::kCross, query.call("jsonb_array_elements", { targets }), {}, "value", { .lateral = true, .withOrdinality = true, .columns = { { .name = "target" }, { .name = "position" } } })
            .where(query.binary(query.column("id", "link"), ruvia::DbBinaryOperator::kEqual, query.value(id)))
            .orderBy(query.column("position", "value"));
        const auto rows = co_await c.db().query(query);
        ruvia::BoxedArray<LinkTargetDto> result(
            ruvia::ModelOptions{ .resource = c.arena() }
        );
        for (const auto& row : rows) {
            auto& target = result.emplace(ruvia::ModelOptions{ .resource = c.arena() });
            const auto targetId = std::string(row[0].value().value_or(std::string_view{}));
            const auto prefix = "target:" + targetId + ':';
            RuntimeDto targetRuntime(ruvia::ModelOptions{ .resource = c.arena() });
            targetRuntime.set<"state">(runtime.text(prefix + "state", "stopped"))
                .template set<"reason">(runtime.text(prefix + "reason"))
                .template set<"error">(runtime.text(prefix + "error"));
            const auto lastActivityAt = runtime.integer(prefix + "last_activity_at_ms");
            if (lastActivityAt > 0) {
                targetRuntime.set<"lastActivityAt">(
                    service::common::utcTimestampFromMilliseconds(lastActivityAt)
                );
            }
            target.set<"id">(targetId)
                .template set<"name">(row[1].value().value_or(std::string_view{}))
                .template set<"ip">(row[2].value().value_or(std::string_view{}))
                .template set<"port">(toInt(row[3].value().value_or(std::string_view{})))
                .template set<"status">(row[4].value().value_or(std::string_view{}))
                .template set<"runtime">(std::move(targetRuntime));
        }
        co_return result;
    }

    template <typename Context>
    static ruvia::Task<RuntimeStatus> loadRuntimeStatus(Context& c, std::string_view id) {
        RuntimeStatus status;
        try {
            const auto owner = co_await c.redis().get("iot:v2:owner:link:" + std::string(id));
            if (!owner) {
                co_return status;
            }
            const auto pattern = "iot:runtime:link:" + std::string(id) + ":worker:" +
                std::string(owner->data(), owner->size()) + ":*";
            std::string cursor = "0";
            std::vector<std::string> keys;
            do {
                const std::array<std::string_view, 6> scanArgs{ "SCAN", cursor, "MATCH", pattern, "COUNT", "100" };
                const auto page = co_await c.redis().command(scanArgs);
                if (page.kind() != ruvia::RedisValue::Kind::kArray || page.array().size() != 2 ||
                    page.array()[0].kind() != ruvia::RedisValue::Kind::kString ||
                    page.array()[1].kind() != ruvia::RedisValue::Kind::kArray) {
                    break;
                }
                cursor.assign(page.array()[0].string());
                for (const auto& value : page.array()[1].array()) {
                    if (value.kind() == ruvia::RedisValue::Kind::kString) {
                        keys.emplace_back(value.string());
                    }
                }
            } while (cursor != "0");

            std::int64_t connectionCount = 0;
            std::int64_t lastActivityAt = 0;
            std::string clients;
            std::string aggregateState = "stopped";
            const auto stateRank = [](std::string_view state) {
                if (state == "connected") {
                    return 6;
                }
                if (state == "listening") {
                    return 5;
                }
                if (state == "reconnecting") {
                    return 4;
                }
                if (state == "connecting") {
                    return 3;
                }
                if (state == "error") {
                    return 2;
                }
                if (state == "idle") {
                    return 1;
                }
                return 0;
            };
            for (const auto& key : keys) {
                const std::array<std::string_view, 2> hashArgs{ "HGETALL", key };
                const auto reply = co_await c.redis().command(hashArgs);
                if (reply.kind() != ruvia::RedisValue::Kind::kArray) {
                    continue;
                }
                RuntimeStatus worker;
                const auto values = reply.array();
                for (std::size_t index = 0; index + 1 < values.size(); index += 2) {
                    if (values[index].kind() != ruvia::RedisValue::Kind::kString ||
                        values[index + 1].kind() != ruvia::RedisValue::Kind::kString) {
                        continue;
                    }
                    worker.fields.insert_or_assign(std::string(values[index].string()), std::string(values[index + 1].string()));
                }
                connectionCount += worker.integer("connection_count");
                lastActivityAt = std::max(lastActivityAt, worker.integer("last_activity_at_ms"));
                const auto endpoints = worker.text("remote_endpoints");
                std::size_t start = 0;
                while (start < endpoints.size()) {
                    const auto end = endpoints.find(',', start);
                    if (!clients.empty()) {
                        clients.push_back('\n');
                    }
                    clients.append(endpoints, start, end == std::string::npos ? std::string::npos : end - start);
                    if (end == std::string::npos) {
                        break;
                    }
                    start = end + 1;
                }
                const auto workerState = worker.text("state", "stopped");
                if (stateRank(workerState) > stateRank(aggregateState)) {
                    aggregateState = workerState;
                }
                if (status.text("state_reason").empty() && !worker.text("state_reason").empty()) {
                    status.fields["state_reason"] = worker.text("state_reason");
                }
                if (status.text("error").empty() && !worker.text("error").empty()) {
                    status.fields["error"] = worker.text("error");
                }
                for (const auto& [name, value] : worker.fields) {
                    if (!name.starts_with("target:")) {
                        continue;
                    }
                    if (!value.empty() || !status.fields.contains(name)) {
                        status.fields[name] = value;
                    }
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

    template <typename Targets>
    static std::string serializeTargets(const Targets& targets) {
        std::string result = "[";
        for (const auto& target : targets) {
            if (result.size() > 1) {
                result.push_back(',');
            }
            result += "{\"id\":";
            appendJsonString(result, target.template get<"id">().view());
            result += ",\"name\":";
            appendJsonString(result, target.template get<"name">().view());
            result += ",\"ip\":";
            appendJsonString(result, target.template get<"ip">().view());
            result += ",\"port\":" +
                std::to_string(static_cast<std::int64_t>(target.template get<"port">()));
            result += ",\"status\":";
            const auto& status = target.template get<"status">();
            appendJsonString(result, status ? status->view() : "enabled");
            result.push_back('}');
        }
        result.push_back(']');
        return result;
    }

    template <typename Targets>
    static std::string serializeEndpoint(std::string_view mode, std::string_view ip, std::int64_t port, const Targets& targets) {
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

    template <typename Context>
    ruvia::Task<void> ensureAvailable(Context& c, const std::string& name, const std::string& mode, const std::string& ip, std::int64_t port, std::optional<std::string> excludedId) {
        ruvia::DbQuery query(c.pool());
        const auto endpointPort = jsonText(query, "endpoint", "port");
        const auto endpointPortText = query.coalesce({ endpointPort, query.value("") });
        const auto endpointPortIsInteger = query.binary(
            endpointPortText,
            ruvia::DbBinaryOperator::kRegex,
            query.value("^[0-9]{1,5}$")
        );
        const auto safeEndpointPort = query.caseWhen(
            { { endpointPortIsInteger, query.cast(endpointPort, ruvia::DbDataType::kInteger) } },
            query.cast(query.value(std::int64_t{ 0 }), ruvia::DbDataType::kInteger)
        );
        const auto nameTaken = query.binary(query.column("name"), ruvia::DbBinaryOperator::kEqual, query.value(name));
        query.select(query.cast(query.value(1), ruvia::DbDataType::kInteger))
            .from(LinkEntity::tableName())
            .where(LinkEntity::column<"deleted_at">().isNull().expression(query));
        if (mode == "TCP Server") {
            const auto serverAddress = query.binary(
                query.binary(query.column("execution"), ruvia::DbBinaryOperator::kEqual, query.value("collector")),
                ruvia::DbBinaryOperator::kAnd,
                query.binary(
                    query.binary(jsonText(query, "endpoint", "mode"), ruvia::DbBinaryOperator::kEqual, query.value(mode)),
                    ruvia::DbBinaryOperator::kAnd,
                    query.binary(
                        query.binary(jsonText(query, "endpoint", "ip"), ruvia::DbBinaryOperator::kEqual, query.value(ip)),
                        ruvia::DbBinaryOperator::kAnd,
                        query.binary(safeEndpointPort, ruvia::DbBinaryOperator::kEqual, query.value(port))
                    )
                )
            );
            query.andWhere(query.binary(nameTaken, ruvia::DbBinaryOperator::kOr, serverAddress));
        } else {
            query.andWhere(nameTaken);
        }
        if (excludedId) {
            query.andWhere(query.binary(query.column("id"), ruvia::DbBinaryOperator::kNotEqual, query.cast(query.value(*excludedId), ruvia::DbDataType::kUuid)));
        }
        query.limit(1);
        const auto rows = co_await c.db().query(query);
        if (!rows.empty()) {
            service::common::fail(15005, "链路名称或监听地址已存在", 409);
        }
    }

    template <typename Context>
    ruvia::Task<void> requireOwner(Context& c, std::string_view ownerId) {
        if (c.userId == ownerId) {
            co_return;
        }
        ruvia::DbQuery query(c.pool());
        query.select(query.cast(query.value(1), ruvia::DbDataType::kInteger))
            .from(service::user::UserRoleEntity::tableName(), "ur")
            .join(ruvia::DbJoinType::kInner, service::role::RoleEntity::tableName(), query.binary(query.column("role_id", "ur"), ruvia::DbBinaryOperator::kEqual, query.column("id", "r")), "r")
            .where(query.binary(query.column("user_id", "ur"), ruvia::DbBinaryOperator::kEqual, query.value(c.userId)))
            .andWhere(query.binary(query.column("code", "r"), ruvia::DbBinaryOperator::kEqual, query.value("superadmin")))
            .andWhere(query.binary(query.column("status", "r"), ruvia::DbBinaryOperator::kEqual, query.value("enabled")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "r")))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(15007, "只能修改或删除自己创建的链路", 403);
        }
    }

    std::string cachedPublicIp_;
    std::chrono::steady_clock::time_point publicIpCachedAt_{};
};

inline LinkService& linkService() {
    return LinkService::instance();
}

} // namespace service::link
