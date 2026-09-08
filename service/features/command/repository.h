#pragma once

#include "service/features/command/queue.h"
#include "service/features/command/state.h"
#include "service/features/access/contract.h"
#include "service/features/edge/dispatch.h"

namespace service::command::repository {

template <typename Database>
ruvia::Task<void> event(Database& db, std::string_view commandId,
                         std::string_view type) {
    const auto eventId = service::common::nextUuidV7();
    (void)co_await db.execute(R"sql(
INSERT INTO outbox_event(id,event_type,aggregate_type,aggregate_id,action,schema_version,payload)
SELECT $1::uuid,$3,'command',device_id::text,'updated',2,
 jsonb_build_object('device_code',device_code,'data',jsonb_build_object(
 'commandId',id::text,'status',status,'reason',reason,'elements',elements,'actualValues',actual_values))
FROM command_operation WHERE id=$2::uuid)sql",
        common::dbParams(eventId, commandId, type));
}

template <typename Database>
ruvia::Task<void> append(Database& db, std::string_view requestId,
                          std::string_view queue, PendingQueueKind kind,
                          const std::vector<PendingDispatch>& dispatches,
                          std::string_view actor, std::size_t maximum,
                          std::string_view nodeId = {}) {
    std::int64_t ordinal = 0;
    for (const auto& dispatch : dispatches) {
        const auto& task = dispatch.task;
        std::string payload = "[";
        if (kind == PendingQueueKind::List) {
            const auto bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(dispatch.listPayload.data()),
                dispatch.listPayload.size());
            payload += access::jsonQuoted(message::toHex(std::vector<std::uint8_t>(bytes.begin(),bytes.end())));
        } else {
            bool first = true;
            for (const auto& field : dispatch.streamFields) {
                if (!first) payload += ',';
                first = false;
                payload += access::jsonQuoted(field.name) + ',' + access::jsonQuoted(field.value);
            }
        }
        payload += ']';
        std::string elements = "[";
        for (const auto& [id,value] : task.elements) {
            if (elements.size()>1) elements += ',';
            elements += "{\"elementId\":" + access::jsonQuoted(id) + ",\"value\":" + access::jsonQuoted(value) + "}";
        }
        elements += ']';
        (void)co_await db.execute(R"sql(
INSERT INTO command_operation(id,request_id,ordinal,device_id,device_code,protocol,status,elements,model_id,model_revision)
SELECT $1::uuid,$2::uuid,$3,$4::uuid,$5,$6,'ACCEPTED',$7::jsonb,protocol_config_id,protocol_revision
FROM device WHERE id=$4::uuid)sql",
            common::dbParams(task.messageId, requestId, ordinal++, task.deviceId,
                             task.deviceCode, task.protocol, elements));
        (void)co_await db.execute(R"sql(
INSERT INTO command_attempt(operation_id,queue_key,queue_kind,payload,submitted_by,node_id,max_length)
VALUES($1::uuid,$2,$3,$4::jsonb,$5,$6,$7))sql",
            common::dbParams(task.messageId, queue,
                             kind == PendingQueueKind::List ? "list" : "stream",
                             payload, actor, nodeId, static_cast<std::int64_t>(maximum)));
        co_await event(db, task.messageId, "device.command.accepted");
    }
}

template <typename Context>
ruvia::Task<std::vector<message::StreamField>> status(Context& context,
                                                     std::string_view id) {
    const auto rows = co_await context.db().query(R"sql(
SELECT device_id::text,device_code,protocol,status,reason,
 (extract(epoch FROM created_at)*1000)::bigint::text,
 COALESCE((extract(epoch FROM completed_at)*1000)::bigint::text,'0')
FROM command_operation WHERE id=$1::uuid)sql", common::dbParams(id));
    std::vector<message::StreamField> fields;
    if (rows.empty()) co_return fields;
    const std::string_view names[]{"device_id","device_code","protocol","status",
                                   "reason","created_at_ms","completed_at_ms"};
    for (std::size_t index = 0; index < 7; ++index)
        fields.push_back({std::string(names[index]),
                          std::string(rows.front()[index].value().value_or(std::string_view{}))});
    const auto actual = co_await context.db().query(R"sql(
SELECT value->>'elementId',value->>'name',value->>'kind',value->>'value',value->>'unit'
FROM command_operation, jsonb_array_elements(actual_values) WITH ORDINALITY a(value,idx)
WHERE id=$1::uuid ORDER BY idx)sql", common::dbParams(id));
    fields.push_back({"actual_value_count",std::to_string(actual.size())});
    const std::string_view actualNames[]{"element_id","name","kind","value","unit"};
    for (std::size_t index = 0; index < actual.size(); ++index)
        for (std::size_t col = 0; col < 5; ++col)
            fields.push_back({"actual_value_" + std::to_string(index) + "_" +
                              std::string(actualNames[col]),
                              std::string(actual[index][col].value().value_or(std::string_view{}))});
    co_return fields;
}

// Claim is committed BEFORE touching the physical delivery path. A crashed or ambiguous
// attempt is never replayed automatically: old EdgeNode cannot promise durable deduplication.
template <typename Context>
ruvia::Task<void> dispatch(Context& context) {
    auto tx = co_await context.db().beginTransaction();
    const auto rows = co_await tx.query(R"sql(
SELECT a.operation_id::text,a.queue_key,a.queue_kind,a.submitted_by,a.node_id,
 a.max_length::text,o.device_id::text,o.device_code,o.protocol,
 (extract(epoch FROM o.created_at)*1000)::bigint::text
FROM command_attempt a JOIN command_operation o ON o.id=a.operation_id
WHERE a.claimed_at IS NULL AND a.deadline>NOW() AND o.status='ACCEPTED'
ORDER BY o.created_at FOR UPDATE OF a,o SKIP LOCKED LIMIT 16)sql");
    for (const auto& row : rows) {
        const auto id = row[0].value().value_or(std::string_view{});
        (void)co_await tx.execute("UPDATE command_attempt SET claimed_at=NOW() WHERE operation_id=$1::uuid",
                                  common::dbParams(id));
        (void)co_await tx.execute("UPDATE command_operation SET status='DISPATCHING' WHERE id=$1::uuid",
                                  common::dbParams(id));
    }
    co_await tx.commit();
    for (const auto& row : rows) {
        const auto cell = [&](std::size_t index) { return row[index].value().value_or(std::string_view{}); };
        std::string failure;
        bool published = false;
        try {
            const auto values = co_await context.db().query(
                "SELECT value FROM command_attempt, jsonb_array_elements_text(payload) WITH ORDINALITY p(value,idx) "
                "WHERE operation_id=$1::uuid ORDER BY idx", common::dbParams(cell(0)));
            PendingDispatch item;
            item.task.messageId = cell(0); item.task.deviceId = cell(6);
            item.task.deviceCode = cell(7); item.task.protocol = cell(8);
            item.task.createdAtMs = common::parseInt64(cell(9)).value_or(0);
            const bool list = cell(2) == "list";
            if (list) {
                const auto bytes = message::fromHex(values.front()[0].value().value_or(std::string_view{}));
                item.listPayload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            } else {
                for (std::size_t index = 0; index + 1 < values.size(); index += 2)
                    item.streamFields.push_back({std::string(values[index][0].value().value_or(std::string_view{})),
                                                  std::string(values[index+1][0].value().value_or(std::string_view{}))});
            }
            published = co_await dispatchPendingBatch(context.redis(), cell(1),
                list ? PendingQueueKind::List : PendingQueueKind::Stream,
                std::vector<PendingDispatch>{std::move(item)}, cell(3),
                static_cast<std::size_t>(common::parseInt64(cell(5)).value_or(1)));
            if (!published) failure = "queue_capacity_exceeded";
        } catch (const std::exception& error) { failure = error.what(); }
        // A known capacity rejection is safe to report as rejected. Other failures can
        // occur after Redis committed the enqueue, so the outcome must stay unknown.
        const std::string_view next = published ? "AWAITING_RESULT" :
            failure == "queue_capacity_exceeded" ? "REJECTED" : "UNKNOWN";
        auto update = co_await context.db().beginTransaction();
        (void)co_await update.execute(R"sql(
UPDATE command_operation SET status=$2,reason=$3,
 completed_at=CASE WHEN $2='AWAITING_RESULT' THEN NULL ELSE NOW() END
WHERE id=$1::uuid AND status='DISPATCHING')sql", common::dbParams(cell(0),next,failure));
        if (published)
            (void)co_await update.execute("UPDATE command_attempt SET dispatched_at=NOW() WHERE operation_id=$1::uuid",
                                          common::dbParams(cell(0)));
        else co_await event(update,cell(0),"device.command.updated");
        co_await update.commit();
        if (published && !cell(4).empty())
            co_await edge::dispatch::notifyNode(context.redis(),cell(4));
    }
    auto expiry = co_await context.db().beginTransaction();
    const auto expired = co_await expiry.query(R"sql(
UPDATE command_operation o SET status=CASE WHEN a.claimed_at IS NULL THEN 'REJECTED' ELSE 'UNKNOWN' END,
 reason=CASE WHEN a.claimed_at IS NULL THEN 'dispatch_deadline_expired' ELSE 'result_not_confirmed' END,
 completed_at=NOW()
FROM command_attempt a WHERE o.id=a.operation_id AND a.deadline<=NOW()
 AND o.status IN ('ACCEPTED','DISPATCHING','AWAITING_RESULT') RETURNING o.id::text)sql");
    for (const auto& row : expired)
        co_await event(expiry,row[0].value().value_or(std::string_view{}),"device.command.updated");
    co_await expiry.commit();
}

} // namespace service::command::repository
