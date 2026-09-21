#pragma once

#include <memory>
#include <map>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/web/ModelObject.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbQuery.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/rpc.h"
#include "service/modules/edge_node/edge_node.service.h"
#include "service/modules/link/link.entity.h"
#include "service/modules/protocol/protocol.entity.h"
#include "service/modules/protocol/protocol.types.h"
#include "service/modules/system/outbox/outbox.service.h"
#include "service/modules/system/role/role.entity.h"
#include "service/modules/system/user/user.entity.h"
#include "service/utils/json.h"
#include "service/common/derived_point.h"

namespace service::protocol {

class ProtocolConfigurationRules final {
  private:
    static std::string trimJson(std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        value = first == std::string_view::npos ? std::string_view{} : value.substr(first);
        while (!value.empty()) {
            const auto ch = value.back();
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                break;
            }
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    static std::optional<std::string> stringValue(const ruvia::JsonValue& value) {
        const auto decoded = value.get<ruvia::String>();
        return decoded ? std::optional<std::string>(decoded->view()) : std::nullopt;
    }

    static std::optional<std::string> stringField(const ruvia::JsonValue& object, std::string_view field) {
        const auto value = object.get<ruvia::JsonValue>(field);
        return value ? stringValue(*value) : std::nullopt;
    }

  public:
    static void validateProtocol(std::string_view protocol) {
        if (protocol != "SL651" && protocol != "Modbus" && protocol != "S7" &&
            protocol != "MC" && protocol != "FINS" && protocol != "DLT645") {
            service::common::fail(16003, "不支持的协议类型", 400);
        }
    }

    static void validateName(std::string_view name) {
        if (name.empty() || name.size() > 64) {
            service::common::fail(16002, "配置名称长度必须在 1 - 64 之间", 400);
        }
    }

  private:
    static bool hasField(const ruvia::JsonValue& object, std::string_view field) {
        return static_cast<bool>(object.get<ruvia::JsonValue>(field));
    }

    static bool oneOf(std::string_view value, std::initializer_list<std::string_view> values) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    static bool enumFieldValid(const ruvia::JsonValue& object, std::string_view field, std::initializer_list<std::string_view> values) {
        const auto raw = object.get<ruvia::JsonValue>(field);
        if (!raw) {
            return true;
        }
        const auto value = stringValue(*raw);
        if (!value) {
            return false;
        }
        return oneOf(*value, values);
    }

    static bool booleanFieldValid(const ruvia::JsonValue& object, std::string_view field) {
        const auto raw = object.get<ruvia::JsonValue>(field);
        return !raw || raw->isBoolean();
    }

    static std::optional<std::string> scalarText(const ruvia::JsonValue& value, bool allowString) {
        if (value.isNumber()) {
            return canonicalNumericText(value.view());
        }
        if (allowString && value.isString()) {
            return stringValue(value);
        }
        return std::nullopt;
    }

    // PostgreSQL jsonb stores numbers as numeric values, so ->> emits a
    // decimal token even when the request used an exponent (for example,
    // 1e3 becomes 1000). Keep that normalization local to numeric JSON
    // values; JSON strings must retain their original token for the integer
    // regex checks below.
    static std::optional<std::string> canonicalNumericText(std::string_view value, std::size_t maximum = 4096) {
        const auto trimmed = trimJson(value);
        std::string_view token = trimmed;
        if (token.empty()) {
            return std::nullopt;
        }

        bool negative = false;
        if (token.front() == '-' || token.front() == '+') {
            negative = token.front() == '-';
            token.remove_prefix(1);
        }
        if (token.empty()) {
            return std::nullopt;
        }

        const auto isDigit = [](char value) {
            return value >= '0' && value <= '9';
        };
        std::size_t position = 0;
        const auto integerStart = position;
        while (position < token.size() && isDigit(token[position])) {
            ++position;
        }
        const auto integerDigits = position - integerStart;
        std::size_t fractionDigits = 0;
        if (position < token.size() && token[position] == '.') {
            ++position;
            const auto fractionStart = position;
            while (position < token.size() && isDigit(token[position])) {
                ++position;
            }
            fractionDigits = position - fractionStart;
        }
        if (integerDigits == 0 && fractionDigits == 0) {
            return std::nullopt;
        }

        std::int64_t exponent = 0;
        if (position < token.size() && (token[position] == 'e' || token[position] == 'E')) {
            ++position;
            bool exponentNegative = false;
            if (position < token.size() && (token[position] == '-' || token[position] == '+')) {
                exponentNegative = token[position] == '-';
                ++position;
            }
            const auto exponentStart = position;
            while (position < token.size() && isDigit(token[position])) {
                ++position;
            }
            if (position == exponentStart) {
                return std::nullopt;
            }
            std::int64_t parsedExponent = 0;
            const auto [end, error] = std::from_chars(token.data() + exponentStart, token.data() + position, parsedExponent);
            if (error != std::errc{} || end != token.data() + position ||
                (exponentNegative && parsedExponent == (std::numeric_limits<std::int64_t>::max)())) {
                return std::nullopt;
            }
            exponent = exponentNegative ? -parsedExponent : parsedExponent;
        }
        if (position != token.size() || integerDigits > maximum ||
            fractionDigits > maximum || integerDigits + fractionDigits > maximum) {
            return std::nullopt;
        }

        std::string digits;
        digits.reserve(integerDigits + fractionDigits);
        digits.append(token, integerStart, integerDigits);
        if (fractionDigits != 0) {
            digits.append(token, integerStart + integerDigits + 1, fractionDigits);
        }

        const auto expansionLimit = static_cast<std::int64_t>(maximum - digits.size());
        if (exponent > expansionLimit || exponent < -expansionLimit) {
            return std::nullopt;
        }
        const auto integerCount = static_cast<std::int64_t>(integerDigits);
        if ((exponent > 0 && integerCount > (std::numeric_limits<std::int64_t>::max)() - exponent) ||
            (exponent < 0 && integerCount < (std::numeric_limits<std::int64_t>::min)() - exponent)) {
            return std::nullopt;
        }
        const auto decimalPosition = integerCount + exponent;

        std::size_t fractionLength = 0;
        if (decimalPosition <= 0) {
            const auto leadingFractionZeros = static_cast<std::size_t>(-decimalPosition);
            if (leadingFractionZeros > maximum - digits.size()) {
                return std::nullopt;
            }
            fractionLength = leadingFractionZeros + digits.size();
        } else if (decimalPosition < static_cast<std::int64_t>(digits.size())) {
            fractionLength = digits.size() - static_cast<std::size_t>(decimalPosition);
        }
        const auto integerLength = decimalPosition <= 0
            ? std::size_t{ 1 }
            : std::max(digits.size(), static_cast<std::size_t>(decimalPosition));
        const auto outputLength = integerLength + (fractionLength == 0 ? 0 : 1 + fractionLength);
        if (outputLength > maximum) {
            return std::nullopt;
        }

        std::string output;
        output.reserve(outputLength + (negative ? 1 : 0));
        if (decimalPosition <= 0) {
            output = "0.";
            output.append(static_cast<std::size_t>(-decimalPosition), '0');
            output += digits;
        } else if (decimalPosition >= static_cast<std::int64_t>(digits.size())) {
            output = digits;
            output.append(static_cast<std::size_t>(decimalPosition) - digits.size(), '0');
        } else {
            const auto split = static_cast<std::size_t>(decimalPosition);
            output.assign(digits.data(), split);
            output.push_back('.');
            output.append(digits.data() + split, digits.size() - split);
        }

        const auto dot = output.find('.');
        const auto integerEnd = dot == std::string::npos ? output.size() : dot;
        const auto firstNonZero = output.find_first_not_of('0', 0);
        if (firstNonZero == std::string::npos || firstNonZero >= integerEnd) {
            output.erase(0, integerEnd > 0 ? integerEnd - 1 : 0);
        } else if (firstNonZero > 0) {
            output.erase(0, firstNonZero);
        }

        bool nonZero = false;
        for (const auto digit : digits) {
            if (digit != '0') {
                nonZero = true;
                break;
            }
        }
        if (negative && nonZero) {
            output.insert(output.begin(), '-');
        }
        return output;
    }

    static int compareAbsoluteNumeric(std::string_view lhs, std::string_view rhs) {
        const auto stripLeadingZeros = [](std::string_view value) {
            const auto first = value.find_first_not_of('0');
            if (first == std::string_view::npos) {
                return std::string_view{ "0" };
            }
            return value.substr(first);
        };
        const auto integerPart = [](std::string_view value) {
            const auto dot = value.find('.');
            return value.substr(0, dot == std::string_view::npos ? value.size() : dot);
        };
        const auto fractionPart = [](std::string_view value) {
            const auto dot = value.find('.');
            return dot == std::string_view::npos ? std::string_view{} : value.substr(dot + 1);
        };
        const auto lhsInteger = stripLeadingZeros(integerPart(lhs));
        const auto rhsInteger = stripLeadingZeros(integerPart(rhs));
        if (lhsInteger.size() != rhsInteger.size()) {
            return lhsInteger.size() < rhsInteger.size() ? -1 : 1;
        }
        if (lhsInteger != rhsInteger) {
            return lhsInteger < rhsInteger ? -1 : 1;
        }

        const auto lhsFraction = fractionPart(lhs);
        const auto rhsFraction = fractionPart(rhs);
        const auto length = std::max(lhsFraction.size(), rhsFraction.size());
        for (std::size_t index = 0; index < length; ++index) {
            const auto left = index < lhsFraction.size() ? lhsFraction[index] : '0';
            const auto right = index < rhsFraction.size() ? rhsFraction[index] : '0';
            if (left != right) {
                return left < right ? -1 : 1;
            }
        }
        return 0;
    }

    static int compareNumeric(std::string_view lhs, std::string_view rhs) {
        const auto left = canonicalNumericText(lhs);
        const auto right = canonicalNumericText(rhs);
        if (!left || !right) {
            return 0;
        }
        const bool leftNegative = !left->empty() && left->front() == '-';
        const bool rightNegative = !right->empty() && right->front() == '-';
        if (leftNegative != rightNegative) {
            return leftNegative ? -1 : 1;
        }
        const auto leftAbsolute = leftNegative ? std::string_view(*left).substr(1) : std::string_view(*left);
        const auto rightAbsolute = rightNegative ? std::string_view(*right).substr(1) : std::string_view(*right);
        const auto result = compareAbsoluteNumeric(leftAbsolute, rightAbsolute);
        return leftNegative ? -result : result;
    }

    static bool unsignedDigits(std::string_view value, std::size_t maximum) {
        if (value.empty() || value.size() > maximum) {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](char ch) {
            return ch >= '0' && ch <= '9';
        });
    }

    static bool signedDigits(std::string_view value, std::size_t maximum) {
        if (!value.empty() && value.front() == '-') {
            value.remove_prefix(1);
        }
        return unsignedDigits(value, maximum);
    }

    static std::optional<std::int64_t> integerFieldValue(
        const std::optional<ruvia::JsonValue>& raw,
        bool allowString,
        std::size_t maximum,
        std::int64_t lower,
        std::int64_t upper
    ) {
        if (!raw) {
            return std::nullopt;
        }
        const auto text = scalarText(*raw, allowString);
        if (!text || !unsignedDigits(*text, maximum)) {
            return std::nullopt;
        }
        std::int64_t value{};
        const auto [end, error] =
            std::from_chars(text->data(), text->data() + text->size(), value);
        if (error != std::errc{} || end != text->data() + text->size() || value < lower ||
            value > upper) {
            return std::nullopt;
        }
        return value;
    }

    static std::optional<std::int64_t> integerFieldValue(
        const ruvia::JsonValue& object,
        std::string_view field,
        bool allowString,
        std::size_t maximum,
        std::int64_t lower,
        std::int64_t upper
    ) {
        return integerFieldValue(object.get<ruvia::JsonValue>(field), allowString, maximum, lower, upper);
    }

    static bool integerFieldValid(const std::optional<ruvia::JsonValue>& raw, bool allowString, std::size_t maximum, std::int64_t lower, std::int64_t upper, bool required = false) {
        if (!raw) {
            return !required;
        }
        return integerFieldValue(raw, allowString, maximum, lower, upper).has_value();
    }

    static bool integerFieldValid(const ruvia::JsonValue& object, std::string_view field, bool allowString, std::size_t maximum, std::int64_t lower, std::int64_t upper, bool required = false) {
        return integerFieldValid(object.get<ruvia::JsonValue>(field), allowString, maximum, lower, upper, required);
    }

    static bool signedIntegerFieldValid(const std::optional<ruvia::JsonValue>& raw, bool allowString, std::size_t maximum, std::int64_t lower, std::int64_t upper) {
        if (!raw) {
            return true;
        }
        const auto text = scalarText(*raw, allowString);
        if (!text || !signedDigits(*text, maximum)) {
            return false;
        }
        std::int64_t value{};
        const auto [end, error] =
            std::from_chars(text->data(), text->data() + text->size(), value);
        return error == std::errc{} && end == text->data() + text->size() && value >= lower &&
            value <= upper;
    }

    static bool signedIntegerFieldValid(const ruvia::JsonValue& object, std::string_view field, bool allowString, std::size_t maximum, std::int64_t lower, std::int64_t upper) {
        return signedIntegerFieldValid(object.get<ruvia::JsonValue>(field), allowString, maximum, lower, upper);
    }

    static bool numberFieldValid(const std::optional<ruvia::JsonValue>& raw, bool allowString, std::string_view lower, std::string_view upper) {
        if (!raw) {
            return true;
        }
        const auto text = scalarText(*raw, allowString);
        if (!text || text->empty()) {
            return false;
        }
        return compareNumeric(*text, lower) >= 0 && compareNumeric(*text, upper) <= 0;
    }

    static bool numberFieldValid(const ruvia::JsonValue& object, std::string_view field, bool allowString, std::string_view lower, std::string_view upper) {
        return numberFieldValid(object.get<ruvia::JsonValue>(field), allowString, lower, upper);
    }

    static bool decimalTokenValid(std::string_view value) {
        std::size_t index = 0;
        if (index < value.size() && (value[index] == '+' || value[index] == '-')) {
            ++index;
        }
        const auto integerStart = index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
            ++index;
        }
        const auto integerDigits = index - integerStart;
        std::size_t fractionDigits = 0;
        bool hasDot = false;
        if (index < value.size() && value[index] == '.') {
            hasDot = true;
            ++index;
            const auto fractionStart = index;
            while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
                ++index;
            }
            fractionDigits = index - fractionStart;
        }
        if ((integerDigits == 0 && fractionDigits == 0) || integerDigits > 18 ||
            fractionDigits > 12) {
            return false;
        }
        if (integerDigits == 0 && !hasDot) {
            return false;
        }
        if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
            ++index;
            if (index < value.size() && (value[index] == '+' || value[index] == '-')) {
                ++index;
            }
            const auto exponentStart = index;
            while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
                ++index;
            }
            if (index == exponentStart || index - exponentStart > 3) {
                return false;
            }
        }
        return index == value.size();
    }

    static bool decimalFieldValid(const std::optional<ruvia::JsonValue>& raw) {
        if (!raw) {
            return true;
        }
        const auto text = scalarText(*raw, true);
        if (!text || !decimalTokenValid(*text)) {
            return false;
        }
        // Compare the exact decimal token so 1000000000.000000000001 does
        // not round down to the inclusive 1e9 limit.
        return compareNumeric(*text, "-1000000000") >= 0 &&
            compareNumeric(*text, "1000000000") <= 0;
    }

    static bool decimalFieldValid(const ruvia::JsonValue& object, std::string_view field) {
        return decimalFieldValid(object.get<ruvia::JsonValue>(field));
    }

    template <typename T>
    static bool modelArray(const std::optional<ruvia::JsonValue>& raw, std::optional<ruvia::Array<T>>& parsed) {
        if (!raw) {
            parsed.reset();
            return true;
        }
        if (!raw->isArray()) {
            return false;
        }
        parsed = raw->template get<ruvia::Array<T>>();
        return parsed.has_value();
    }

    static bool enumValueValid(const std::optional<ruvia::String>& value, std::initializer_list<std::string_view> values) {
        return !value || oneOf(value->view(), values);
    }

  public:
    static void validateConfig(const std::optional<ruvia::JsonValue>& config, const std::string& protocol, bool required) {
        if (!config) {
            if (required) {
                service::common::fail(16004, "config 不能为空", 400);
            }
            return;
        }
        if (!config->isObject()) {
            service::common::fail(16004, "config 必须是对象", 400);
        }
        if (hasField(*config, "storageInterval")) {
            service::common::fail(16004, "配置不允许 storageInterval，请使用 storagePolicy", 400);
        }
        if (!enumFieldValid(*config, "storagePolicy", { "report", "change" })) {
            service::common::fail(16004, "配置的 storagePolicy 无效", 400);
        }
        if (required && !hasField(*config, "storagePolicy")) {
            service::common::fail(16004, "配置的 storagePolicy 不能为空", 400);
        }

        if (protocol == "SL651") {
            if (!enumFieldValid(*config, "responseMode", { "M1", "M2", "M3", "M4" }) ||
                (hasField(*config, "funcs") &&
                 !config->get<ruvia::JsonValue>("funcs")->isArray()) ||
                (required && (!hasField(*config, "responseMode") || !hasField(*config, "funcs")))) {
                service::common::fail(16004, "SL651 配置无效", 400);
            }
            std::optional<ruvia::Array<Sl651Func>> funcs;
            if (!modelArray(config->get<ruvia::JsonValue>("funcs"), funcs)) {
                service::common::fail(16004, "SL651 配置无效", 400);
            }
            const auto validElement = [](const Sl651Element& element) {
                if (!enumValueValid(element.get<"positionMode">(), { "GUIDE", "OFFSET" })) {
                    return false;
                }
                const auto position = element.get<"positionMode">() ? std::string(element.get<"positionMode">()->view()) : std::string("GUIDE");
                if (position == "OFFSET") {
                    const auto offset = element.isPresent<"byteOffset">()
                        ? integerFieldValue(element.get<"byteOffset">(), false, 7, 0, 8388607)
                        : std::optional<std::int64_t>{ 0 };
                    const auto length = integerFieldValue(element.get<"length">(), false, 7, 1, 8388608);
                    return offset && length && *offset + *length <= 8388608 &&
                        integerFieldValid(element.get<"digits">(), false, 1, 0, 7, true) &&
                        enumValueValid(element.get<"encode">(), { "BCD", "HEX", "DICT", "JPEG", "TIME_YYMMDDHHMMSS" }) &&
                        element.isPresent<"encode">();
                }
                if (position != "GUIDE") {
                    return false;
                }
                const auto guide = element.get<"guideHex">() ? std::optional<std::string>(element.get<"guideHex">()->view()) : std::nullopt;
                const auto encoding = element.get<"encode">() ? std::optional<std::string>(element.get<"encode">()->view()) : std::nullopt;
                if (!guide || (guide->size() != 4 && guide->size() != 6) ||
                    !std::all_of(guide->begin(), guide->end(), [](unsigned char ch) {
                        return std::isxdigit(ch) != 0;
                    }) ||
                    !encoding || !oneOf(*encoding, { "BCD", "HEX", "DICT", "JPEG", "TIME_YYMMDDHHMMSS" }) || !integerFieldValid(element.get<"length">(), false, 7, 0, 8388608, true) || !integerFieldValid(element.get<"digits">(), false, 1, 0, 7, true)) {
                    return false;
                }
                const auto lead = std::stoi(guide->substr(0, 2), nullptr, 16);
                if ((lead == 0xFF) != (guide->size() == 6)) {
                    return false;
                }
                const auto definition = std::stoi(guide->substr(guide->size() - 2), nullptr, 16);
                const auto length = std::stoi(*scalarText(*element.get<"length">(), false));
                const auto digits = std::stoi(*scalarText(*element.get<"digits">(), false));
                if (lead < 0xF0 || lead == 0xFF) {
                    return length > 0 && length == (definition >> 3) &&
                        (*encoding != "BCD" || digits == (definition & 7));
                }
                if (definition != lead) {
                    return false;
                }
                if (lead == 0xF0) {
                    return length == 5;
                }
                if (lead == 0xF1) {
                    return length == 6;
                }
                return lead == 0xF2 || lead == 0xF3 || length > 0;
            };
            if (funcs) {
                for (const auto& function : *funcs) {
                    const auto code = function.get<"funcCode">() ? std::optional<std::string>(function.get<"funcCode">()->view()) : std::nullopt;
                    if (!code || code->size() != 2 || !std::all_of(code->begin(), code->end(), [](unsigned char ch) {
                            return std::isxdigit(ch) != 0;
                        }) ||
                        !enumValueValid(function.get<"dir">(), { "UP", "DOWN" })) {
                        service::common::fail(16004, "SL651 引导符、长度或小数位不符合数据定义", 400);
                    }
                    for (const auto* elements : { &function.get<"elements">(), &function.get<"responseElements">() }) {
                        if (!*elements) {
                            continue;
                        }
                        for (const auto& element : **elements) {
                            if (!validElement(element)) {
                                service::common::fail(16004, "SL651 引导符、长度或小数位不符合数据定义", 400);
                            }
                        }
                    }
                }
            }
            return;
        }

        if (protocol == "MC" || protocol == "FINS" || protocol == "DLT645") {
            const auto connectionValue = config->get<ruvia::JsonValue>("connection");
            const auto pointsValue = config->get<ruvia::JsonValue>("points");
            const auto connection = connectionValue ? connectionValue->get<IndustrialConnection>() : std::optional<IndustrialConnection>{};
            std::optional<ruvia::Array<IndustrialPoint>> points;
            if (!connectionValue || !connectionValue->isObject() || !connection ||
                !modelArray(pointsValue, points) || !points ||
                !integerFieldValid(*config, "readInterval", true, 5, 1, 3600) ||
                !integerFieldValid(*config, "commandFastReadDuration", true, 5, 0, 3600) ||
                !integerFieldValid(*config, "commandFastReadInterval", true, 5, 1, 3600)) {
                service::common::fail(16004, "协议连接、点位或采集周期配置无效", 400);
            }
            if (protocol == "MC") {
                if (!enumValueValid(connection->get<"frame">(), { "3E", "4E" }) ||
                    !integerFieldValid(connection->get<"network">(), false, 3, 0, 255) ||
                    !integerFieldValid(connection->get<"station">(), false, 3, 0, 255) ||
                    !integerFieldValid(connection->get<"moduleIo">(), false, 5, 0, 65535) ||
                    !integerFieldValid(connection->get<"multidrop">(), false, 3, 0, 255) ||
                    !integerFieldValid(connection->get<"monitoringTimer">(), false, 5, 1, 65535)) {
                    service::common::fail(16004, "MC/SLMP 连接参数无效", 400);
                }
            } else if (protocol == "FINS") {
                const std::pair<std::string_view, std::int64_t> routes[] = {
                    { "destinationNetwork", 127 }, { "sourceNetwork", 127 },
                    { "destinationNode", 254 }, { "sourceNode", 254 },
                    { "destinationUnit", 255 }, { "sourceUnit", 255 },
                };
                const auto validRoute = [&](std::string_view field, std::int64_t maximum) {
                    if (field == "destinationNetwork") return integerFieldValid(connection->get<"destinationNetwork">(), false, 3, 0, maximum);
                    if (field == "sourceNetwork") return integerFieldValid(connection->get<"sourceNetwork">(), false, 3, 0, maximum);
                    if (field == "destinationNode") return integerFieldValid(connection->get<"destinationNode">(), false, 3, 0, maximum);
                    if (field == "sourceNode") return integerFieldValid(connection->get<"sourceNode">(), false, 3, 0, maximum);
                    if (field == "destinationUnit") return integerFieldValid(connection->get<"destinationUnit">(), false, 3, 0, maximum);
                    return integerFieldValid(connection->get<"sourceUnit">(), false, 3, 0, maximum);
                };
                for (const auto& [field, maximum] : routes) {
                    if (!validRoute(field, maximum)) {
                        service::common::fail(16004, "FINS 路由参数无效", 400);
                    }
                }
            } else {
                if (!enumValueValid(connection->get<"version">(), { "1997", "2007" }) ||
                    !integerFieldValid(connection->get<"wakeupBytes">(), false, 1, 0, 4)) {
                    service::common::fail(16004, "DL/T645 版本或唤醒字节数无效", 400);
                }
                for (const auto* field : { &connection->get<"writePassword">(), &connection->get<"operatorCode">() }) {
                    if (!*field) {
                        continue;
                    }
                    const auto text = std::string((*field)->view());
                    if (!text.empty() && (text.size() != 8 || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
                                                         return std::isxdigit(c) != 0;
                                                     }))) {
                        service::common::fail(16004, "DL/T645 写入认证字段必须为空或 8 位十六进制", 400);
                    }
                }
            }
            std::set<std::string> ids;
            if (points->size() > 256) {
                service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
            }
            for (const auto& point : *points) {
                const auto id = point.get<"id">() ? std::optional<std::string>(point.get<"id">()->view()) : std::nullopt;
                const auto name = point.get<"name">() ? std::optional<std::string>(point.get<"name">()->view()) : std::nullopt;
                const auto type = point.get<"dataType">() ? std::optional<std::string>(point.get<"dataType">()->view()) : std::nullopt;
                const auto unit = point.get<"unit">() ? std::optional<std::string>(point.get<"unit">()->view()) : std::nullopt;
                if (!id || !service::common::isUuid(*id) || !ids.insert(*id).second || !name || name->empty() || name->size() > 100 || !type ||
                    (unit && unit->size() > 32) ||
                    (point.isPresent<"writable">() && !point.get<"writable">())) {
                    service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
                }
                if (protocol == "DLT645") {
                    const auto identifier = point.get<"identifier">() ? std::optional<std::string>(point.get<"identifier">()->view()) : std::nullopt;
                    const auto version = connection->get<"version">() ? std::string(connection->get<"version">()->view()) : std::string("2007");
                    if (!identifier || identifier->size() != (version == "1997" ? 4 : 8) ||
                        !std::all_of(identifier->begin(), identifier->end(), [](unsigned char c) {
                            return std::isxdigit(c) != 0;
                        }) ||
                        !oneOf(*type, { "BCD", "BCD_SIGNED", "HEX" }) || !integerFieldValid(point.get<"length">(), false, 3, 1, *type == "HEX" ? 200 : 8, true) || !integerFieldValid(point.get<"digits">(), false, 1, 0, 8)) {
                        service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
                    }
                    if (point.get<"writable">() && static_cast<bool>(*point.get<"writable">())) {
                        const auto password = connection->get<"writePassword">() ? std::string(connection->get<"writePassword">()->view()) : std::string{};
                        const auto operatorCode = connection->get<"operatorCode">() ? std::string(connection->get<"operatorCode">()->view()) : std::string{};
                        if (password.size() != 8 || (version == "2007" && operatorCode.size() != 8)) {
                            service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
                        }
                        const auto length = integerFieldValue(point.get<"length">(), false, 3, 1, 200);
                        if (!length || *length > (version == "2007" ? 38 : 44)) {
                            service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
                        }
                    }
                    continue;
                }
                const auto area = point.get<"area">() ? std::optional<std::string>(point.get<"area">()->view()) : std::nullopt;
                const auto bits = *type == "BOOL";
                if (!area || !oneOf(*type, { "BOOL", "INT16", "UINT16", "INT32", "UINT32", "FLOAT32", "INT64", "UINT64", "DOUBLE" }) ||
                    !enumValueValid(point.get<"byteOrder">(), { "BIG_ENDIAN", "LITTLE_ENDIAN", "BIG_ENDIAN_BYTE_SWAP", "LITTLE_ENDIAN_BYTE_SWAP" }) ||
                    !decimalFieldValid(point.get<"scale">()) || !signedIntegerFieldValid(point.get<"decimals">(), false, 2, -1, 8)) {
                    service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
                }
                const auto address = integerFieldValue(point.get<"address">(), false, 8, 0, protocol == "MC" ? 16777215 : 65535);
                const auto width = bits ? 1 : (*type == "INT16" || *type == "UINT16") ? 1
                    : (*type == "INT64" || *type == "UINT64" || *type == "DOUBLE")    ? 4
                                                                                      : 2;
                if (!address) {
                    service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
                }
                bool ok = false;
                if (protocol == "MC") {
                    const bool bitArea = oneOf(*area, { "M", "X", "Y", "B", "L", "F", "V", "S", "TS", "CS" });
                    ok = (bitArea || oneOf(*area, { "D", "W", "R", "ZR", "TN", "CN" })) && (!bits || bitArea) &&
                        *address + width * (bitArea && !bits ? 16 : 1) - 1 <= 16777215;
                } else {
                    ok = oneOf(*area, { "D", "CIO", "W", "H", "A" }) && *address + (bits ? 0 : width - 1) <= 65535 &&
                        integerFieldValid(point.get<"bit">(), false, 2, 0, bits ? 15 : 0);
                }
                if (!ok) {
                    service::common::fail(16004, "协议点位配置无效，请检查地址、类型、长度、唯一标识及写入认证", 400);
                }
            }
            return;
        }

        if (protocol == "S7") {
            const auto connectionValue = config->get<ruvia::JsonValue>("connection");
            const auto areasValue = config->get<ruvia::JsonValue>("areas");
            const auto connection = connectionValue && connectionValue->isObject() ? connectionValue->get<S7Connection>() : std::optional<S7Connection>{};
            std::optional<ruvia::Array<S7Area>> areas;
            if (!enumFieldValid(*config, "plcModel", { "S7-200", "S7-300", "S7-400", "S7-1200", "S7-1500" }) ||
                (connectionValue && (!connectionValue->isObject() || !connection)) ||
                (areasValue && !modelArray(areasValue, areas)) ||
                (required && (!hasField(*config, "plcModel") || !connection || !areas))) {
                service::common::fail(16004, "S7 配置无效", 400);
            }
            if (connection &&
                (!enumValueValid(connection->get<"probeMode">(), { "STANDARD", "COMPATIBLE", "AUTO" }) ||
                 !numberFieldValid(connection->get<"handshakeTimeout">(), false, "1000", "30000") ||
                 !numberFieldValid(connection->get<"directProbeTimeout">(), false, "1000", "30000"))) {
                service::common::fail(16004, "S7 配置无效", 400);
            }
            if (hasField(*config, "pollInterval")) {
                service::common::fail(16004, "S7 配置不允许 pollInterval，请使用 readInterval", 400);
            }
            if (!integerFieldValid(*config, "readInterval", true, 5, 1, 3600)) {
                service::common::fail(16004, "S7 配置的 readInterval 无效", 400);
            }
            if (areas) {
                for (const auto& area : *areas) {
                    const auto id = area.get<"id">() ? std::optional<std::string>(area.get<"id">()->view()) : std::nullopt;
                    const auto name = area.get<"name">() ? std::optional<std::string>(area.get<"name">()->view()) : std::nullopt;
                    const auto areaType = area.get<"area">() ? std::optional<std::string>(area.get<"area">()->view()) : std::nullopt;
                    if (!id || id->empty() || !name || name->empty() || !areaType ||
                        !oneOf(*areaType, { "DB", "V", "MK", "PE", "PA", "CT", "TM" })) {
                        service::common::fail(16004, "S7 寄存器配置无效", 400);
                    }
                    if (!enumValueValid(area.get<"dataType">(), { "BOOL", "INT8", "UINT8", "INT16", "UINT16", "INT32", "UINT32", "FLOAT", "LREAL", "STRING" }) ||
                        !integerFieldValid(area.get<"start">(), false, 10, 0, 2147483647, true) ||
                        !integerFieldValid(area.get<"size">(), false, 5, 1, 65535, true) ||
                        !integerFieldValid(area.get<"startBit">(), false, 1, 0, 7) ||
                        !signedIntegerFieldValid(area.get<"decimals">(), false, 2, -1, 8) ||
                        (area.isPresent<"writable">() && !area.get<"writable">())) {
                        service::common::fail(16004, "S7 寄存器配置无效", 400);
                    }
                    if (*areaType == "DB" && !integerFieldValid(area.get<"dbNumber">(), false, 5, 1, 65535, true)) {
                        service::common::fail(16004, "S7 寄存器配置无效", 400);
                    }
                }
            }
            return;
        }

        if (protocol != "Modbus") {
            return;
        }
        const auto packetValue = config->get<ruvia::JsonValue>("packet");
        const auto registersValue = config->get<ruvia::JsonValue>("registers");
        const auto packet = packetValue && packetValue->isObject() ? packetValue->get<ModbusPacket>() : std::optional<ModbusPacket>{};
        std::optional<ruvia::Array<ModbusRegister>> registers;
        if (!enumFieldValid(*config, "byteOrder", { "BIG_ENDIAN", "LITTLE_ENDIAN", "BIG_ENDIAN_BYTE_SWAP", "LITTLE_ENDIAN_BYTE_SWAP" }) ||
            (required && !hasField(*config, "byteOrder"))) {
            service::common::fail(16004, "Modbus 配置的 byteOrder 无效", 400);
        }
        if ((registersValue && !modelArray(registersValue, registers)) || (required && !registers)) {
            service::common::fail(16004, "Modbus 配置的 registers 必须是数组", 400);
        }
        if (packetValue && (!packetValue->isObject() || !packet)) {
            service::common::fail(16004, "Modbus 配置的 packet 必须是对象", 400);
        }
        if (!integerFieldValid(*config, "readInterval", true, 5, 1, 3600)) {
            service::common::fail(16004, "Modbus 配置的 readInterval 无效", 400);
        }
        if (packet && !integerFieldValid(packet->get<"mergeGap">(), true, 5, 0, 2000)) {
            service::common::fail(16004, "Modbus 配置的 packet.mergeGap 无效", 400);
        }
        if (packet && !integerFieldValid(packet->get<"maxQuantity">(), true, 5, 1, 125)) {
            service::common::fail(16004, "Modbus 配置的 packet.maxQuantity 无效", 400);
        }
        if (hasField(*config, "pollInterval")) {
            service::common::fail(16004, "Modbus 配置不允许 pollInterval，请使用 readInterval", 400);
        }
        if (!registers) {
            return;
        }
        for (const auto& entry : *registers) {
            const auto id = entry.get<"id">() ? std::optional<std::string>(entry.get<"id">()->view()) : std::nullopt;
            const auto name = entry.get<"name">() ? std::optional<std::string>(entry.get<"name">()->view()) : std::nullopt;
            const auto registerType = entry.get<"registerType">() ? std::optional<std::string>(entry.get<"registerType">()->view()) : std::nullopt;
            const auto dataType = entry.get<"dataType">() ? std::optional<std::string>(entry.get<"dataType">()->view()) : std::nullopt;
            if (!id || id->empty() || !name || name->empty() || !registerType || !dataType ||
                !oneOf(*registerType, { "COIL", "DISCRETE_INPUT", "HOLDING_REGISTER", "INPUT_REGISTER" }) ||
                !oneOf(*dataType, { "BOOL", "INT16", "UINT16", "INT32", "UINT32", "FLOAT32", "INT64", "UINT64", "DOUBLE" }) ||
                !integerFieldValid(entry.get<"address">(), false, 5, 0, 65535, true) ||
                !integerFieldValid(entry.get<"quantity">(), false, 1, 1, 4, true)) {
                service::common::fail(16004, "Modbus 寄存器配置无效", 400);
            }
            if (!enumValueValid(entry.get<"byteOrder">(), { "BIG_ENDIAN", "LITTLE_ENDIAN", "BIG_ENDIAN_BYTE_SWAP", "LITTLE_ENDIAN_BYTE_SWAP" })) {
                service::common::fail(16004, "Modbus 寄存器 byteOrder 无效", 400);
            }
            if (!decimalFieldValid(entry.get<"scale">())) {
                service::common::fail(16004, "Modbus 寄存器 scale 无效", 400);
            }
            if (!signedIntegerFieldValid(entry.get<"decimals">(), true, 2, -1, 8)) {
                service::common::fail(16004, "Modbus 寄存器 decimals 无效", 400);
            }
            if (entry.isPresent<"writable">() && !entry.get<"writable">()) {
                service::common::fail(16004, "Modbus 寄存器 writable 必须是布尔值", 400);
            }
        }
    }
};


class ProtocolService {
    static void validateDerivedConfig(const ruvia::JsonValue& config) {
        try { (void)service::common::orderDerivedPoints(config); }
        catch (const std::invalid_argument& error) { service::common::fail(16004, error.what(), 400); }
    }
  public:
    static ExpressionTestResult testExpression(const ExpressionTestBody& body) {
        try {
            std::map<std::string, double> inputs;
            for (const auto& input : body.get<"inputs">()) {
                const std::string alias(input.get<"alias">().view());
                const double value = input.get<"value">().value;
                const utils::Expression variable(alias);
                if (variable.variables().size() != 1 || variable.variables().front() != alias ||
                    !std::isfinite(value) || !inputs.emplace(alias, value).second) {
                    throw std::invalid_argument("变量名无效、重复或测试值不是有限数值");
                }
            }
            const auto compile = [&](std::string_view text) {
                utils::Expression expression(text);
                for (const auto& alias : expression.variables()) {
                    if (!inputs.contains(alias)) throw std::invalid_argument("未提供变量测试值：" + alias);
                }
                return expression;
            };
            const auto expression = compile(body.get<"expression">().view());
            std::vector<utils::Expression> conditions;
            for (const auto& rule : body.get<"unitRules">()) conditions.push_back(compile(rule.get<"condition">().view()));
            const auto resolve = [&](std::string_view alias) { return inputs.at(std::string(alias)); };
            const auto value = expression.evaluate(resolve);
            std::string unit(body.get<"unit">().view());
            std::int64_t matched{};
            for (std::size_t i = 0; i < conditions.size(); ++i) {
                if (conditions[i].evaluate(resolve) != 0) {
                    unit = body.get<"unitRules">()[i].get<"unit">().view();
                    matched = static_cast<std::int64_t>(i + 1);
                    break;
                }
            }
            ExpressionTestResult result;
            result.set<"value">(value).set<"unit">(unit).set<"matchedRule">(matched);
            return result;
        } catch (const std::invalid_argument& error) {
            common::fail(16004, error.what(), 400);
        } catch (const std::domain_error& error) {
            common::fail(16004, error.what(), 400);
        }
    }

    static ProtocolService& instance() {
        static thread_local ProtocolService service;
        return service;
    }

    template <typename Context>
    ruvia::Task<std::string> list(Context& c, std::int64_t page, std::int64_t pageSize, std::optional<std::string> protocol) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 1000);
        ruvia::DbFindOptions countOptions;
        countOptions.where = ProtocolConfigEntity::column<"deleted_at">().isNull();
        if (protocol && !protocol->empty()) {
            countOptions.where = std::move(countOptions.where) &&
                ProtocolConfigEntity::column<"protocol">() == *protocol;
        }
        const auto total = co_await c.db().template getRepository<ProtocolConfigEntity>().count(countOptions);

        auto query = protocolSelect(c.pool());
        if (protocol && !protocol->empty()) {
            query.andWhere(query.binary(query.column("protocol"), ruvia::DbBinaryOperator::kEqual, query.value(*protocol)));
        }
        query.orderBy(query.column("id"), ruvia::DbOrderDirection::kDesc)
            .limit(static_cast<std::uint64_t>(pageSize))
            .offset(static_cast<std::uint64_t>((page - 1) * pageSize));
        const auto rows = co_await c.db().query(query);

        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows) {
            if (!first) {
                result.push_back(',');
            }
            first = false;
            result += itemJson(row);
        }
        result += "],\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
            ",\"pageSize\":" + std::to_string(pageSize) + ",\"totalPages\":" +
            std::to_string(total == 0 ? 0 : (total + pageSize - 1) / pageSize) + "}";
        co_return result;
    }

    template <typename Context>
    ruvia::Task<std::string> detail(Context& c, std::string_view id) {
        co_return co_await detailData(c, id);
    }

    template <typename Context>
    ruvia::Task<std::string> options(Context& c, const std::string& protocol, std::int64_t page, std::int64_t pageSize) {
        page = std::max<std::int64_t>(1, page);
        pageSize = std::clamp<std::int64_t>(pageSize, 1, 1000);
        ruvia::DbFindOptions options;
        options.where = ProtocolConfigEntity::column<"deleted_at">().isNull() &&
            ProtocolConfigEntity::column<"protocol">() == protocol;
        const auto total = co_await c.db().template getRepository<ProtocolConfigEntity>().count(options);
        options.where = std::move(options.where) && ProtocolConfigEntity::column<"enabled">() == true;
        options.order = { { "name" } };
        options.take = static_cast<std::uint64_t>(pageSize);
        options.skip = static_cast<std::uint64_t>((page - 1) * pageSize);
        const auto rows = co_await c.db().template getRepository<ProtocolConfigEntity>().find(options);
        std::string result = "{\"list\":[";
        bool first = true;
        for (const auto& row : rows) {
            if (!first) {
                result.push_back(',');
            }
            first = false;
            result += "{\"id\":" + service::utils::jsonQuoted(row.template get<"id">()) +
                ",\"name\":" + service::utils::jsonQuoted(row.template get<"name">()) + "}";
        }
        result += "],\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
            ",\"pageSize\":" + std::to_string(pageSize) + "}";
        co_return result;
    }

    template <typename Context>
    ruvia::Task<void> create(Context& c, const CreateProtocolBody& body) {
        const std::string protocol(body.get<"protocol">().view());
        const std::string name(body.get<"name">().view());
        ProtocolConfigurationRules::validateProtocol(protocol);
        ProtocolConfigurationRules::validateName(name);
        if (body.isNull<"config">()) service::common::fail(16004, "config 必须是对象", 400);
        ProtocolConfigurationRules::validateConfig(body.get<"config">(), protocol, true);
        validateDerivedConfig(*body.get<"config">());
        co_await ensureNameAvailable(c, name, std::nullopt);
        const auto id = c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next();
        const auto& config = body.get<"config">();
        const auto& remark = body.get<"remark">();
        const bool enabled = body.get<"enabled">().value_or(ruvia::Bool{true}).value;
        auto transaction = co_await c.db().beginTransaction();
        ProtocolConfigEntity configuration(c.pool());
        configuration.set<"id">(id);
        configuration.set<"protocol">(protocol);
        configuration.set<"name">(name);
        configuration.set<"enabled">(enabled);
        configuration.set<"config">(config->view());
        configuration.set<"created_by">(c.userId);
        if (remark && !remark->view().empty()) {
            configuration.set<"remark">(remark->view());
        } else {
            configuration.setNull<"remark">();
        }
        (void)co_await transaction.template getRepository<ProtocolConfigEntity>().insert(configuration);
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "protocol", "created", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
    }

    template <typename Context>
    ruvia::Task<void> update(Context& c, std::string_view id, const UpdateProtocolBody& body) {
        ruvia::DbFindOptions options;
        options.where = service::common::database::activeId<ProtocolConfigEntity>(id);
        const auto existing = co_await c.db().template getRepository<ProtocolConfigEntity>().findOne(options);
        if (!existing) {
            service::common::fail(16001, "协议配置不存在", 404);
        }
        co_await requireOwner(c, existing->template get<"created_by">());
        const std::string protocol(existing->template get<"protocol">());
        if (const auto& requested = body.get<"protocol">()) {
            if (requested->view().empty()) {
                service::common::fail(16003, "protocol 不能为空", 400);
            }
            if (requested->view() != protocol) {
                service::common::fail(16006, "协议类型不可修改", 409);
            }
        }
        if (const auto& name = body.get<"name">()) {
            ProtocolConfigurationRules::validateName(name->view());
            co_await ensureNameAvailable(c, std::string(name->view()), std::string(id));
        }
        auto transaction = co_await c.db().beginTransaction();
        options.lock = ruvia::DbLockOptions{};
        const auto locked = co_await transaction.template getRepository<ProtocolConfigEntity>().findOne(options);
        if (!locked) service::common::fail(16001, "协议配置不存在", 404);
        if (body.isNull<"config">()) service::common::fail(16004, "config 必须是对象", 400);
        ProtocolConfigurationRules::validateConfig(body.get<"config">(), protocol, false);
        if (body.get<"config">()) {
            const auto previous = ruvia::JsonValue::parse(locked->template get<"config">());
            std::map<std::string, std::string> fields;
            if (!previous) throw std::runtime_error("invalid stored protocol configuration");
            (void)previous->forEachField([&](std::string_view key, const ruvia::JsonValue& value) {
                fields[std::string(key)] = std::string(value.view());
                return true;
            });
            (void)body.get<"config">()->forEachField([&](std::string_view key, const ruvia::JsonValue& value) {
                fields[std::string(key)] = std::string(value.view());
                return true;
            });
            std::string merged = "{";
            for (const auto& [key, value] : fields) {
                if (merged.size() > 1) merged += ',';
                merged += service::utils::jsonQuoted(key) + ':' + value;
            }
            merged += '}';
            const auto mergedConfig = ruvia::JsonValue::parse(merged);
            validateDerivedConfig(*mergedConfig);
            if (!service::common::orderDerivedPoints(*mergedConfig).empty()) co_await requireDerivedSupport(transaction, c.pool(), id);
        }
        const auto& name = body.get<"name">();
        const auto& remark = body.get<"remark">();
        const auto& config = body.get<"config">();
        const auto& enabled = body.get<"enabled">();
        ruvia::DbExpressions expressions(c.pool());
        std::vector<ruvia::DbAssignment> changes{ { "updated_at", expressions.call("now") } };
        if (name) {
            changes.push_back({ "name", expressions.value(name->view()) });
        }
        if (enabled) {
            changes.push_back({ "enabled", expressions.value(enabled->value) });
        }
        if (config) {
            changes.push_back({ "config", expressions.binary(expressions.column("config"), ruvia::DbBinaryOperator::kJsonConcat, expressions.cast(expressions.value(config->view()), ruvia::DbDataType::kJsonb)) });
        }
        if (body.isPresent<"remark">()) {
            changes.push_back({ "remark", remark && !remark->view().empty() ? expressions.value(remark->view()) : expressions.nullValue() });
        }
        (void)co_await transaction.template getRepository<ProtocolConfigEntity>().update(
            service::common::database::activeId<ProtocolConfigEntity>(id),
            changes
        );
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "protocol", "updated", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
        try {
            (void)co_await service::rpc::call(c, "telemetry", "project-protocol", std::string(id));
        } catch (...) {
            // Startup hydration repairs Redis read models if Redis is temporarily unavailable.
        }
        try {
            co_await syncEdgeNodes(c, id);
        } catch (const std::exception& error) {
            logPostUpdateFailure("edge-sync", id, error.what());
        } catch (...) {
            logPostUpdateFailure("edge-sync", id, "unknown exception");
        }
    }

    template <typename Context>
    ruvia::Task<void> remove(Context& c, std::string_view id) {
        ruvia::DbFindOptions options;
        options.where = service::common::database::activeId<ProtocolConfigEntity>(id);
        const auto existing = co_await c.db().template getRepository<ProtocolConfigEntity>().findOne(options);
        if (!existing) {
            service::common::fail(16001, "协议配置不存在", 404);
        }
        co_await requireOwner(c, existing->template get<"created_by">());
        ruvia::DbFindOptions used;
        used.where = entities::DeviceEntity::column<"protocol_config_id">() == id &&
            entities::DeviceEntity::column<"deleted_at">().isNull();
        if (co_await c.db().template getRepository<entities::DeviceEntity>().exists(used)) {
            service::common::fail(16008, "协议配置已被设备使用，请先删除关联设备", 409);
        }
        auto transaction = co_await c.db().beginTransaction();
        ruvia::DbExpressions expressions(c.pool());
        (void)co_await transaction.template getRepository<ProtocolConfigEntity>().update(
            service::common::database::activeId<ProtocolConfigEntity>(id),
            { { "deleted_at", expressions.call("now") }, { "updated_at", expressions.call("now") } }
        );
        co_await service::system::OutboxService::enqueueConfigEvent(transaction, "protocol", "deleted", id, c.template workerState<std::unique_ptr<service::common::UuidV7Generator>>()->next());
        co_await transaction.commit();
    }

  private:
    template <typename Context>
    static ruvia::Task<std::string> detailData(Context& c, std::string_view id) {
        auto query = protocolSelect(c.pool());
        query.where(ProtocolConfigEntity::column<"deleted_at">().isNull().expression(query))
            .andWhere(query.binary(query.column("id"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(id), ruvia::DbDataType::kUuid)))
            .limit(1);
        const auto rows = co_await c.db().query(query);
        if (rows.empty()) {
            service::common::fail(16001, "协议配置不存在", 404);
        }
        co_return itemJson(rows.front());
    }

    static ruvia::DbQuery edgeNodeReferences(std::pmr::memory_resource* resource, std::string_view configId) {
        ruvia::DbQuery query(resource);
        const auto edgeNodeId =
            query.cast(query.column("edge_node_id", "l"), ruvia::DbDataType::kText);
        query.select(edgeNodeId)
            .from(service::protocol::entities::DeviceEntity::tableName(), "d")
            .join(ruvia::DbJoinType::kInner, service::link::LinkEntity::tableName(), query.binary(query.column("id", "l"), ruvia::DbBinaryOperator::kEqual, query.column(service::protocol::entities::DeviceEntity::columnName<"link_id">(), "d")), "l")
            .where(query.binary(query.column(service::protocol::entities::DeviceEntity::columnName<"protocol_config_id">(), "d"), ruvia::DbBinaryOperator::kEqual, query.cast(query.value(configId), ruvia::DbDataType::kUuid)))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column(service::protocol::entities::DeviceEntity::columnName<"deleted_at">(), "d")))
            .andWhere(query.binary(query.column("execution", "l"), ruvia::DbBinaryOperator::kEqual, query.value("edge")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNull, query.column("deleted_at", "l")))
            .andWhere(query.unary(ruvia::DbUnaryOperator::kIsNotNull, query.column("edge_node_id", "l")))
            .distinct()
            .orderBy(edgeNodeId);
        return query;
    }

    static ruvia::Task<void> requireDerivedSupport(ruvia::DbTransaction& transaction, std::pmr::memory_resource* resource, std::string_view configId) {
        const auto rows = co_await transaction.query(edgeNodeReferences(resource, configId));
        for (const auto& row : rows) {
            const auto nodeId = row[0].value().value_or(std::string_view{});
            ruvia::DbFindOptions nodeOptions;
            nodeOptions.where = service::edge::EdgeNodeEntity::column<"id">() == nodeId;
            const auto node = co_await transaction.template getRepository<service::edge::EdgeNodeEntity>().findOne(nodeOptions);
            const auto capability = node ? ruvia::JsonValue::parse(node->template get<"capability">()) : std::optional<ruvia::JsonValue>{};
            const auto supported = capability ? capability->template get<ruvia::Bool>("derivedPoints") : std::optional<ruvia::Bool>{};
            if (!supported || !static_cast<bool>(*supported)) service::common::fail(16009, "关联边缘节点尚不支持派生点，请先升级固件", 409);
        }
    }

    template <typename Context>
    static ruvia::Task<void> syncEdgeNodes(Context& c, std::string_view configId) {
        const auto rows = co_await c.db().query(edgeNodeReferences(c.pool(), configId));
        for (const auto& row : rows) {
            const auto nodeId = row[0].value().value_or(std::string_view{});
            if (nodeId.empty()) continue;
            try {
                (void)co_await service::edge::EdgeService::queueSnapshot(c, nodeId, c.userId);
            } catch (const std::exception& error) {
                std::cerr << "protocol edge config sync failed: node=" << nodeId
                          << " config=" << configId << " error=" << error.what() << '\n';
            } catch (...) {
                std::cerr << "protocol edge config sync failed: node=" << nodeId
                          << " config=" << configId << " error=unknown exception\n";
            }
        }
    }

    static void logPostUpdateFailure(std::string_view stage, std::string_view configId, std::string_view message) {
        std::cerr << "protocol post-update " << stage << " failed: config=" << configId
                  << " error=" << message << '\n';
    }

    static ruvia::DbQuery protocolSelect(std::pmr::memory_resource* resource) {
        ruvia::DbQuery query(resource);
        query.select({ query.cast(query.column("id"), ruvia::DbDataType::kText), query.column("protocol"), query.column("name"), query.column("enabled"), query.column("config"), service::common::database::emptyText(query, "remark"), query.cast(query.call("iot_utc_timestamp", { query.column("created_at") }), ruvia::DbDataType::kText), query.cast(query.call("iot_utc_timestamp", { query.column("updated_at") }), ruvia::DbDataType::kText) })
            .from(ProtocolConfigEntity::tableName())
            .where(ProtocolConfigEntity::column<"deleted_at">().isNull().expression(query));
        return query;
    }

    static std::string itemJson(const ruvia::DbRow& row) {
        const auto enabled = row[3].as<bool>().value_or(false);
        std::string result = "{\"id\":" + service::utils::jsonQuoted(row[0].value().value_or("")) +
            ",\"protocol\":" +
            service::utils::jsonQuoted(row[1].value().value_or("")) +
            ",\"name\":" + service::utils::jsonQuoted(row[2].value().value_or("")) +
            ",\"enabled\":" + (enabled ? "true" : "false") +
            ",\"config\":" + std::string(row[4].value().value_or("{}")) +
            ",\"remark\":" + service::utils::jsonQuoted(row[5].value().value_or("")) +
            ",\"created_at\":" +
            service::utils::jsonQuoted(row[6].value().value_or("")) +
            ",\"updated_at\":" +
            service::utils::jsonQuoted(row[7].value().value_or("")) + "}";
        return result;
    }

    template <typename Context>
    ruvia::Task<void> ensureNameAvailable(Context& c, const std::string& name, std::optional<std::string> excludedId) {
        ruvia::DbFindOptions options;
        options.where = ProtocolConfigEntity::column<"deleted_at">().isNull() &&
            ProtocolConfigEntity::column<"name">() == name;
        if (excludedId) {
            options.where = std::move(options.where) && ProtocolConfigEntity::column<"id">() != *excludedId;
        }
        if (co_await c.db().template getRepository<ProtocolConfigEntity>().exists(options)) {
            service::common::fail(16005, "配置名称已存在", 409);
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
            service::common::fail(16007, "只能修改或删除自己创建的协议配置", 403);
        }
    }
};

inline ProtocolService& protocolService() {
    return ProtocolService::instance();
}

} // namespace service::protocol
