#pragma once

#include <ruvia/web/Model.h>

namespace service::link {

RUVIA_REQUEST_MODEL(LinkTargetBody, RUVIA_FIELD(id, ruvia::String),
                    RUVIA_FIELD(name, ruvia::String), RUVIA_FIELD(ip, ruvia::String),
                    RUVIA_FIELD(port, ruvia::Int64), RUVIA_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(SaveLinkBody, RUVIA_FIELD(name, ruvia::String),
                    RUVIA_FIELD(mode, ruvia::String), RUVIA_FIELD(protocol, ruvia::String),
                    RUVIA_FIELD(ip, ruvia::String), RUVIA_FIELD(port, ruvia::Int64),
                    RUVIA_FIELD(targets, ruvia::Array<LinkTargetBody>),
                    RUVIA_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(LinkListQuery, RUVIA_FIELD(page, ruvia::Int64, RUVIA_DEFAULT(1)),
                    RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64, RUVIA_DEFAULT(10)),
                    RUVIA_FIELD(keyword, ruvia::String), RUVIA_FIELD(mode, ruvia::String),
                    RUVIA_FIELD(protocol, ruvia::String), RUVIA_FIELD(status, ruvia::String));

RUVIA_REQUEST_MODEL(LinkIdParams, RUVIA_FIELD(id, ruvia::Int64));

RUVIA_RESPONSE_MODEL(LinkTargetDto, RUVIA_FIELD(id, ruvia::String),
                     RUVIA_FIELD(name, ruvia::String), RUVIA_FIELD(ip, ruvia::String),
                     RUVIA_FIELD(port, ruvia::Int64), RUVIA_FIELD(status, ruvia::String));

RUVIA_RESPONSE_MODEL(LinkItemDto, RUVIA_FIELD(id, ruvia::Int64), RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD(mode, ruvia::String), RUVIA_FIELD(protocol, ruvia::String),
                     RUVIA_FIELD(ip, ruvia::String), RUVIA_FIELD(port, ruvia::Int64),
                     RUVIA_FIELD(targets, ruvia::List<LinkTargetDto>),
                     RUVIA_FIELD(status, ruvia::String),
                     RUVIA_FIELD_NAME("conn_status", connStatus, ruvia::String),
                     RUVIA_FIELD_NAME("client_count", clientCount, ruvia::Int64),
                     RUVIA_FIELD_NAME("created_by", createdBy, ruvia::Int64),
                     RUVIA_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(LinkOptionDto, RUVIA_FIELD(id, ruvia::Int64), RUVIA_FIELD(name, ruvia::String),
                     RUVIA_FIELD(mode, ruvia::String), RUVIA_FIELD(protocol, ruvia::String));

RUVIA_RESPONSE_MODEL(LinkEnumsDto, RUVIA_FIELD(modes, ruvia::List<ruvia::String>),
                     RUVIA_FIELD(protocols, ruvia::List<ruvia::String>),
                     RUVIA_FIELD(statuses, ruvia::List<ruvia::String>));

RUVIA_RESPONSE_MODEL(LinkPageDataDto, RUVIA_FIELD(list, ruvia::List<LinkItemDto>),
                     RUVIA_FIELD(total, ruvia::Int64), RUVIA_FIELD(page, ruvia::Int64),
                     RUVIA_FIELD_NAME("pageSize", pageSize, ruvia::Int64),
                     RUVIA_FIELD_NAME("totalPages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(LinkPageResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, LinkPageDataDto));
RUVIA_RESPONSE_MODEL(LinkDetailResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, LinkItemDto));
RUVIA_RESPONSE_MODEL(LinkOptionsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String),
                     RUVIA_FIELD(data, ruvia::List<LinkOptionDto>));
RUVIA_RESPONSE_MODEL(LinkEnumsResponse, RUVIA_FIELD(code, ruvia::Int64),
                     RUVIA_FIELD(message, ruvia::String), RUVIA_FIELD(data, LinkEnumsDto));

} // namespace service::link
