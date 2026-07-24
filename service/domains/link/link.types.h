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

struct LinkEndpointBody final {
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(targets, ruvia::Array<LinkTargetBody>);
    RUVIA_MODEL(LinkEndpointBody, mode, ip, port, targets);
};

struct SaveLinkBody final {
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointBody);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_MODEL(SaveLinkBody, name, protocol, endpoint, status);
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

struct RuntimeDto final {
    RUVIA_OPTIONAL_FIELD(state, ruvia::String);
    RUVIA_OPTIONAL_FIELD(reason, ruvia::String);
    RUVIA_OPTIONAL_FIELD(error, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("clientCount", clientCount, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(clients, ruvia::List<ruvia::String>);
    RUVIA_OPTIONAL_FIELD_NAME("lastActivityAt", lastActivityAt, ruvia::Int64);
    RUVIA_MODEL(RuntimeDto, state, reason, error, clientCount, clients, lastActivityAt);
};

struct LinkTargetDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(runtime, RuntimeDto);
    RUVIA_MODEL(LinkTargetDto, id, name, ip, port, status, runtime);
};

struct LinkEndpointDto final {
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String);
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String);
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(targets, ruvia::List<LinkTargetDto>);
    RUVIA_MODEL(LinkEndpointDto, mode, ip, port, targets);
};

struct LinkItemDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointDto);
    RUVIA_OPTIONAL_FIELD(status, ruvia::String);
    RUVIA_OPTIONAL_FIELD(runtime, RuntimeDto);
    RUVIA_OPTIONAL_FIELD_NAME("created_by", createdBy, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String);
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String);
    RUVIA_MODEL(LinkItemDto, id, name, protocol, endpoint, status, runtime, createdBy, createdAt,
                updatedAt);
};

struct LinkOptionDto final {
    RUVIA_OPTIONAL_FIELD(id, ruvia::String);
    RUVIA_OPTIONAL_FIELD(name, ruvia::String);
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String);
    RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointDto);
    RUVIA_MODEL(LinkOptionDto, id, name, protocol, endpoint);
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
