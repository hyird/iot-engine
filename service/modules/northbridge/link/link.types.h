#pragma once

#include <ruvia/web/Model.h>

namespace service::link {

struct LinkTargetBody final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(LinkTargetBody, id, name, ip, port, status);
};

struct SaveLinkBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(targets, ruvia::Array<LinkTargetBody>);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(SaveLinkBody, name, mode, protocol, ip, port, targets, status);
};

struct LinkListQuery final {
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1));
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10));
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String);
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(LinkListQuery, page, pageSize, keyword, mode, protocol, status);
};

struct LinkIdParams final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_MODEL(LinkIdParams, id);
};

struct LinkTargetDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("conn_status", connStatus, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("state_reason", stateReason, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("error_msg", errorMessage, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("last_activity_at_ms", lastActivityAtMs, ruvia::Int64);
    RUVIA_MODEL(LinkTargetDto, id, name, ip, port, status, connStatus, stateReason, errorMessage, lastActivityAtMs);
};

struct LinkItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(targets, ruvia::List<LinkTargetDto>);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("conn_status", connStatus, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("client_count", clientCount, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("state_reason", stateReason, ruvia::String);
    RUVIA_OPTIONAL_FIELD(clients, ruvia::List<ruvia::String>);
    RUVIA_OPTIONAL_FIELD_NAME("error_msg", errorMessage, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("created_by", createdBy, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_MODEL(LinkItemDto, id, name, mode, protocol, ip, port, targets, status, connStatus,
                clientCount, stateReason, clients, errorMessage, createdBy, createdAt, updatedAt);
};

struct LinkOptionDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_MODEL(LinkOptionDto, id, name, mode, protocol);
};

struct LinkEnumsDto final {
    RUVIA_OPTIONAL_FIELD(modes, ruvia::List<ruvia::String>);
    RUVIA_OPTIONAL_FIELD(protocols, ruvia::List<ruvia::String>);
    RUVIA_OPTIONAL_FIELD(statuses, ruvia::List<ruvia::String>);
    RUVIA_MODEL(LinkEnumsDto, modes, protocols, statuses);
};

struct LinkPageDataDto final {
    RUVIA_OPTIONAL_FIELD(list, ruvia::List<LinkItemDto>);
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64);
    RUVIA_MODEL(LinkPageDataDto, list, total, page, pageSize, totalPages);
};

struct LinkPageResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, LinkPageDataDto);
    RUVIA_MODEL(LinkPageResponse, code, message, data);
};
struct LinkDetailResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, LinkItemDto);
    RUVIA_MODEL(LinkDetailResponse, code, message, data);
};
struct LinkOptionsResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, ruvia::List<LinkOptionDto>);
    RUVIA_MODEL(LinkOptionsResponse, code, message, data);
};
struct LinkEnumsResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, LinkEnumsDto);
    RUVIA_MODEL(LinkEnumsResponse, code, message, data);
};
struct PublicIpDto final {
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_MODEL(PublicIpDto, ip);
};
struct PublicIpResponse final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_OPTIONAL_FIELD(data, PublicIpDto);
    RUVIA_MODEL(PublicIpResponse, code, message, data);
};

} // namespace service::link
