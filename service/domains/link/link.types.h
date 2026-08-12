#pragma once

#include <ruvia/web/Model.h>

namespace service::link {

RUVIA_REQUEST_MODEL(LinkTargetBody,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String),
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(LinkEndpointBody,
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String),
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(targets, ruvia::Array<LinkTargetBody>));

RUVIA_REQUEST_MODEL(SaveLinkBody,
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String),
    RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointBody),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(LinkListQuery,
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)),
    RUVIA_OPTIONAL_FIELD(keyword, ruvia::String),
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(LinkIdParams,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String));

RUVIA_RESPONSE_MODEL(RuntimeDto,
    RUVIA_OPTIONAL_FIELD(state, ruvia::String),
    RUVIA_OPTIONAL_FIELD(reason, ruvia::String),
    RUVIA_OPTIONAL_FIELD(error, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("clientCount", clientCount, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(clients, ruvia::BoxedArray<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("lastActivityAt", lastActivityAt, ruvia::String));

RUVIA_RESPONSE_MODEL(LinkTargetDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String),
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(runtime, RuntimeDto));

RUVIA_RESPONSE_MODEL(LinkEndpointDto,
    RUVIA_OPTIONAL_FIELD(mode, ruvia::String),
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String),
    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(targets, ruvia::BoxedArray<LinkTargetDto>));

RUVIA_RESPONSE_MODEL(LinkItemDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String),
    RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointDto),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(runtime, RuntimeDto),
    RUVIA_OPTIONAL_FIELD_NAME("created_by", createdBy, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(LinkOptionDto,
    RUVIA_OPTIONAL_FIELD(id, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String),
    RUVIA_OPTIONAL_FIELD(endpoint, LinkEndpointDto));

RUVIA_RESPONSE_MODEL(LinkEnumsDto,
    RUVIA_OPTIONAL_FIELD(modes, ruvia::BoxedArray<ruvia::String>),
    RUVIA_OPTIONAL_FIELD(protocols, ruvia::BoxedArray<ruvia::String>),
    RUVIA_OPTIONAL_FIELD(statuses, ruvia::BoxedArray<ruvia::String>));

RUVIA_RESPONSE_MODEL(LinkPageDataDto,
    RUVIA_OPTIONAL_FIELD(list, ruvia::BoxedArray<LinkItemDto>),
    RUVIA_OPTIONAL_FIELD(total, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(page, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(LinkPageResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, LinkPageDataDto));
RUVIA_RESPONSE_MODEL(LinkDetailResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, LinkItemDto));
RUVIA_RESPONSE_MODEL(LinkOptionsResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, ruvia::BoxedArray<LinkOptionDto>));
RUVIA_RESPONSE_MODEL(LinkEnumsResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, LinkEnumsDto));
RUVIA_RESPONSE_MODEL(PublicIpDto,
    RUVIA_OPTIONAL_FIELD(ip, ruvia::String));
RUVIA_RESPONSE_MODEL(PublicIpResponse,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(data, PublicIpDto));

} // namespace service::link
