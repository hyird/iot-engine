#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::collector::mqtt {

RUVIA_MODEL(Connection, RUVIA_REQUIRED_FIELD(clientId, ruvia::String), RUVIA_OPTIONAL_FIELD(username, ruvia::String), RUVIA_OPTIONAL_FIELD(password, ruvia::String), RUVIA_OPTIONAL_FIELD(keepAliveSeconds, ruvia::Int64));
RUVIA_MODEL(ValueMapping, RUVIA_REQUIRED_FIELD(input, ruvia::String), RUVIA_REQUIRED_FIELD(output, ruvia::String));
RUVIA_MODEL(Point, RUVIA_REQUIRED_FIELD(id, ruvia::String), RUVIA_REQUIRED_FIELD(name, ruvia::String), RUVIA_REQUIRED_FIELD(field, ruvia::String), RUVIA_REQUIRED_FIELD(dataType, ruvia::String), RUVIA_OPTIONAL_FIELD(unit, ruvia::String), RUVIA_OPTIONAL_FIELD(writable, ruvia::Bool), RUVIA_OPTIONAL_FIELD(scale, ruvia::Double), RUVIA_OPTIONAL_FIELD(offset, ruvia::Double), RUVIA_OPTIONAL_FIELD(enumValues, ruvia::Array<ValueMapping>));
RUVIA_MODEL(Config, RUVIA_REQUIRED_FIELD(topic, ruvia::String), RUVIA_OPTIONAL_FIELD(recordsPath, ruvia::String), RUVIA_OPTIONAL_FIELD(deviceCodeField, ruvia::String), RUVIA_OPTIONAL_FIELD(identitySource, ruvia::String), RUVIA_OPTIONAL_FIELD(topicDeviceSegment, ruvia::Int64), RUVIA_OPTIONAL_FIELD(payloadFormat, ruvia::String), RUVIA_OPTIONAL_FIELD(delimiter, ruvia::String), RUVIA_OPTIONAL_FIELD(recordDelimiter, ruvia::String), RUVIA_OPTIONAL_FIELD(recordLength, ruvia::Int64), RUVIA_OPTIONAL_FIELD(timeField, ruvia::String), RUVIA_OPTIONAL_FIELD(timeFormat, ruvia::String), RUVIA_OPTIONAL_FIELD(reportTemplate, ruvia::String), RUVIA_OPTIONAL_FIELD(commandTopic, ruvia::String), RUVIA_OPTIONAL_FIELD(commandTemplate, ruvia::String), RUVIA_OPTIONAL_FIELD(qos, ruvia::Int64), RUVIA_REQUIRED_FIELD(points, ruvia::Array<Point>));

} // namespace service::collector::mqtt
