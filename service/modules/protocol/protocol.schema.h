#pragma once

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <ruvia/web/ModelObject.h>
#include <ruvia/web/detail/json/JsonString.h>
#include <ruvia/web/detail/json/JsonSkip.h>
#include "service/utils/json.h"

#include <ruvia/web/Controller.h>

#include "service/modules/protocol/protocol.types.h"
#include "service/common/http.h"

namespace service::protocol {

class ProtocolListQueryValidator final : public ruvia::Middleware<ProtocolListQueryValidator> {
  public:
    RUVIA_VALIDATE_QUERY(ProtocolListQuery, RUVIA_RULE(page, RUVIA_MIN(1, "page 必须大于 0")),
                         RUVIA_RULE_NAME("pageSize", pageSize,
                                         RUVIA_MIN(1, "pageSize 必须在 1 - 1000 之间"),
                                         RUVIA_MAX(1000, "pageSize 必须在 1 - 1000 之间")),
                         RUVIA_RULE(protocol, RUVIA_ONE_OF("协议无效", "SL651", "Modbus", "S7")))
};

class ProtocolIdParamsValidator final : public ruvia::Middleware<ProtocolIdParamsValidator> {
  public:
    RUVIA_VALIDATE_PARAM(ProtocolIdParams,
                         RUVIA_RULE(id, RUVIA_REQUIRED("id 不能为空"),
                                    RUVIA_CUSTOM("id 必须是 UUID", service::common::isUuidField)))
};

} // namespace service::protocol

namespace service::protocol {

class ProtocolPayloadValidator final {
  private:
    static std::string trimJson(std::string_view value) {
        ruvia::detail::skipJsonWhitespace(value);
        while (!value.empty()) {
            const auto ch = value.back();
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
                break;
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    static std::optional<std::string> stringValue(const ruvia::JsonValue& value) {
        if (!value.isString())
            return std::nullopt;
        auto input = value.view();
        const auto token = ruvia::detail::parseJsonString(input);
        ruvia::detail::skipJsonWhitespace(input);
        if (!token || !input.empty())
            return std::nullopt;
        if (token->encoding() == ruvia::detail::JsonStringEncoding::kLiteral)
            return std::string(token->raw());
        const auto decoded =
            ruvia::detail::decodeJsonString(token->raw(), std::pmr::get_default_resource());
        return decoded ? std::optional<std::string>(std::string(*decoded)) : std::nullopt;
    }

    static std::optional<std::string> stringField(const ruvia::JsonValue& object,
                                                  std::string_view field) {
        const auto value = service::utils::jsonField(object, field);
        return value ? stringValue(*value) : std::nullopt;
    }

  public:
    static std::optional<std::string> nullableString(const ruvia::JsonValue& object,
                                                      std::string_view field) {
        const auto value = service::utils::jsonField(object, field);
        if (!value || value->isNull())
            return std::nullopt;
        const auto result = stringValue(*value);
        if (!result)
            service::common::fail(16002, std::string(field) + " 必须是字符串或 null", 400);
        if (result->size() > 500)
            service::common::fail(16002, std::string(field) + " 长度超出限制", 400);
        if (result->empty())
            return std::nullopt;
        return result;
    }

    static std::optional<bool> optionalBool(const ruvia::JsonValue& object,
                                            std::string_view field) {
        const auto value = service::utils::jsonField(object, field);
        if (!value)
            return std::nullopt;
        if (!value->isBoolean())
            service::common::fail(16004, "enabled 必须是布尔值", 400);
        return trimJson(value->view()) == "true";
    }

    static std::string requiredString(const ruvia::JsonValue& payload, std::string_view field,
                                      std::string_view message) {
        const auto value = payload.get<ruvia::String>(field);
        if (!value || value->view().empty())
            service::common::fail(16002, std::string(message), 400);
        return std::string(value->view());
    }

    static std::optional<std::string> optionalString(const ruvia::JsonValue& payload,
                                                     std::string_view field,
                                                     std::string_view typeMessage,
                                                     std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw)
            return std::nullopt;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(16002, std::string(typeMessage), 400);
        if (value->view().size() > maximum)
            service::common::fail(16002, std::string(field) + " 长度超出限制", 400);
        return std::string(value->view());
    }

    static void validateOptionalNullableString(const ruvia::JsonValue& payload,
                                               std::string_view field,
                                               std::string_view typeMessage,
                                               std::size_t maximum) {
        const auto raw = service::utils::jsonField(payload, field);
        if (!raw || raw->isNull())
            return;
        const auto value = payload.get<ruvia::String>(field);
        if (!value)
            service::common::fail(16002, std::string(typeMessage), 400);
        if (value->view().size() > maximum)
            service::common::fail(16002, std::string(field) + " 长度超出限制", 400);
    }

    static void validateProtocol(std::string_view protocol) {
        if (protocol != "SL651" && protocol != "Modbus" && protocol != "S7")
            service::common::fail(16003, "不支持的协议类型", 400);
    }

    static void validateName(std::string_view name) {
        if (name.empty() || name.size() > 64)
            service::common::fail(16002, "配置名称长度必须在 1 - 64 之间", 400);
    }

  private:
    static bool hasField(const ruvia::JsonValue& object, std::string_view field) {
        return static_cast<bool>(service::utils::jsonField(object, field));
    }

    static bool oneOf(std::string_view value,
                      std::initializer_list<std::string_view> values) {
        return std::find(values.begin(), values.end(), value) != values.end();
    }

    static bool enumFieldValid(const ruvia::JsonValue& object, std::string_view field,
                               std::initializer_list<std::string_view> values) {
        const auto raw = service::utils::jsonField(object, field);
        if (!raw)
            return true;
        const auto value = stringValue(*raw);
        if (!value)
            return false;
        return oneOf(*value, values);
    }

    static bool booleanFieldValid(const ruvia::JsonValue& object, std::string_view field) {
        const auto raw = service::utils::jsonField(object, field);
        return !raw || raw->isBoolean();
    }

    static std::optional<std::string> scalarText(const ruvia::JsonValue& value,
                                                 bool allowString) {
        if (value.isNumber())
            return canonicalNumericText(value.view());
        if (allowString && value.isString())
            return stringValue(value);
        return std::nullopt;
    }

    // PostgreSQL jsonb stores numbers as numeric values, so ->> emits a
    // decimal token even when the request used an exponent (for example,
    // 1e3 becomes 1000). Keep that normalization local to numeric JSON
    // values; JSON strings must retain their original token for the integer
    // regex checks below.
    static std::optional<std::string> canonicalNumericText(std::string_view value,
                                                            std::size_t maximum = 4096) {
        const auto trimmed = trimJson(value);
        std::string_view token = trimmed;
        if (token.empty())
            return std::nullopt;

        bool negative = false;
        if (token.front() == '-' || token.front() == '+') {
            negative = token.front() == '-';
            token.remove_prefix(1);
        }
        if (token.empty())
            return std::nullopt;

        const auto isDigit = [](char value) {
            return value >= '0' && value <= '9';
        };
        std::size_t position = 0;
        const auto integerStart = position;
        while (position < token.size() && isDigit(token[position]))
            ++position;
        const auto integerDigits = position - integerStart;
        std::size_t fractionDigits = 0;
        if (position < token.size() && token[position] == '.') {
            ++position;
            const auto fractionStart = position;
            while (position < token.size() && isDigit(token[position]))
                ++position;
            fractionDigits = position - fractionStart;
        }
        if (integerDigits == 0 && fractionDigits == 0)
            return std::nullopt;

        std::int64_t exponent = 0;
        if (position < token.size() && (token[position] == 'e' || token[position] == 'E')) {
            ++position;
            bool exponentNegative = false;
            if (position < token.size() && (token[position] == '-' || token[position] == '+')) {
                exponentNegative = token[position] == '-';
                ++position;
            }
            const auto exponentStart = position;
            while (position < token.size() && isDigit(token[position]))
                ++position;
            if (position == exponentStart)
                return std::nullopt;
            std::int64_t parsedExponent = 0;
            const auto [end, error] = std::from_chars(token.data() + exponentStart,
                                                      token.data() + position,
                                                      parsedExponent);
            if (error != std::errc{} || end != token.data() + position ||
                (exponentNegative && parsedExponent == (std::numeric_limits<std::int64_t>::max)()))
                return std::nullopt;
            exponent = exponentNegative ? -parsedExponent : parsedExponent;
        }
        if (position != token.size() || integerDigits > maximum ||
            fractionDigits > maximum || integerDigits + fractionDigits > maximum)
            return std::nullopt;

        std::string digits;
        digits.reserve(integerDigits + fractionDigits);
        digits.append(token, integerStart, integerDigits);
        if (fractionDigits != 0)
            digits.append(token, integerStart + integerDigits + 1, fractionDigits);

        const auto expansionLimit = static_cast<std::int64_t>(maximum - digits.size());
        if (exponent > expansionLimit || exponent < -expansionLimit)
            return std::nullopt;
        const auto integerCount = static_cast<std::int64_t>(integerDigits);
        if ((exponent > 0 && integerCount > (std::numeric_limits<std::int64_t>::max)() - exponent) ||
            (exponent < 0 && integerCount < (std::numeric_limits<std::int64_t>::min)() - exponent))
            return std::nullopt;
        const auto decimalPosition = integerCount + exponent;

        std::size_t fractionLength = 0;
        if (decimalPosition <= 0) {
            const auto leadingFractionZeros = static_cast<std::size_t>(-decimalPosition);
            if (leadingFractionZeros > maximum - digits.size())
                return std::nullopt;
            fractionLength = leadingFractionZeros + digits.size();
        } else if (decimalPosition < static_cast<std::int64_t>(digits.size())) {
            fractionLength = digits.size() - static_cast<std::size_t>(decimalPosition);
        }
        const auto integerLength = decimalPosition <= 0
                                        ? std::size_t{1}
                                        : std::max(digits.size(), static_cast<std::size_t>(decimalPosition));
        const auto outputLength = integerLength + (fractionLength == 0 ? 0 : 1 + fractionLength);
        if (outputLength > maximum)
            return std::nullopt;

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
        if (firstNonZero == std::string::npos || firstNonZero >= integerEnd)
            output.erase(0, integerEnd > 0 ? integerEnd - 1 : 0);
        else if (firstNonZero > 0)
            output.erase(0, firstNonZero);

        bool nonZero = false;
        for (const auto digit : digits) {
            if (digit != '0') {
                nonZero = true;
                break;
            }
        }
        if (negative && nonZero)
            output.insert(output.begin(), '-');
        return output;
    }

    static int compareAbsoluteNumeric(std::string_view lhs, std::string_view rhs) {
        const auto stripLeadingZeros = [](std::string_view value) {
            const auto first = value.find_first_not_of('0');
            if (first == std::string_view::npos)
                return std::string_view{"0"};
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
        if (lhsInteger.size() != rhsInteger.size())
            return lhsInteger.size() < rhsInteger.size() ? -1 : 1;
        if (lhsInteger != rhsInteger)
            return lhsInteger < rhsInteger ? -1 : 1;

        const auto lhsFraction = fractionPart(lhs);
        const auto rhsFraction = fractionPart(rhs);
        const auto length = std::max(lhsFraction.size(), rhsFraction.size());
        for (std::size_t index = 0; index < length; ++index) {
            const auto left = index < lhsFraction.size() ? lhsFraction[index] : '0';
            const auto right = index < rhsFraction.size() ? rhsFraction[index] : '0';
            if (left != right)
                return left < right ? -1 : 1;
        }
        return 0;
    }

    static int compareNumeric(std::string_view lhs, std::string_view rhs) {
        const auto left = canonicalNumericText(lhs);
        const auto right = canonicalNumericText(rhs);
        if (!left || !right)
            return 0;
        const bool leftNegative = !left->empty() && left->front() == '-';
        const bool rightNegative = !right->empty() && right->front() == '-';
        if (leftNegative != rightNegative)
            return leftNegative ? -1 : 1;
        const auto leftAbsolute = leftNegative ? std::string_view(*left).substr(1) : std::string_view(*left);
        const auto rightAbsolute = rightNegative ? std::string_view(*right).substr(1) : std::string_view(*right);
        const auto result = compareAbsoluteNumeric(leftAbsolute, rightAbsolute);
        return leftNegative ? -result : result;
    }

    static bool unsignedDigits(std::string_view value, std::size_t maximum) {
        if (value.empty() || value.size() > maximum)
            return false;
        return std::all_of(value.begin(), value.end(),
                           [](char ch) { return ch >= '0' && ch <= '9'; });
    }

    static bool signedDigits(std::string_view value, std::size_t maximum) {
        if (!value.empty() && value.front() == '-')
            value.remove_prefix(1);
        return unsignedDigits(value, maximum);
    }

    static std::optional<std::int64_t> integerFieldValue(
        const ruvia::JsonValue& object, std::string_view field, bool allowString,
        std::size_t maximum, std::int64_t lower, std::int64_t upper) {
        const auto raw = service::utils::jsonField(object, field);
        if (!raw)
            return std::nullopt;
        const auto text = scalarText(*raw, allowString);
        if (!text || !unsignedDigits(*text, maximum))
            return std::nullopt;
        std::int64_t value{};
        const auto [end, error] =
            std::from_chars(text->data(), text->data() + text->size(), value);
        if (error != std::errc{} || end != text->data() + text->size() || value < lower ||
            value > upper)
            return std::nullopt;
        return value;
    }

    static bool integerFieldValid(const ruvia::JsonValue& object, std::string_view field,
                                  bool allowString, std::size_t maximum, std::int64_t lower,
                                  std::int64_t upper, bool required = false) {
        if (!hasField(object, field))
            return !required;
        return integerFieldValue(object, field, allowString, maximum, lower, upper).has_value();
    }

    static bool signedIntegerFieldValid(const ruvia::JsonValue& object, std::string_view field,
                                        bool allowString, std::size_t maximum,
                                        std::int64_t lower, std::int64_t upper) {
        const auto raw = service::utils::jsonField(object, field);
        if (!raw)
            return true;
        const auto text = scalarText(*raw, allowString);
        if (!text || !signedDigits(*text, maximum))
            return false;
        std::int64_t value{};
        const auto [end, error] =
            std::from_chars(text->data(), text->data() + text->size(), value);
        return error == std::errc{} && end == text->data() + text->size() && value >= lower &&
               value <= upper;
    }

    static bool numberFieldValid(const ruvia::JsonValue& object, std::string_view field,
                                 bool allowString, std::string_view lower,
                                 std::string_view upper) {
        const auto raw = service::utils::jsonField(object, field);
        if (!raw)
            return true;
        const auto text = scalarText(*raw, allowString);
        if (!text || text->empty())
            return false;
        return compareNumeric(*text, lower) >= 0 && compareNumeric(*text, upper) <= 0;
    }

    static bool decimalTokenValid(std::string_view value) {
        std::size_t index = 0;
        if (index < value.size() && (value[index] == '+' || value[index] == '-'))
            ++index;
        const auto integerStart = index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9')
            ++index;
        const auto integerDigits = index - integerStart;
        std::size_t fractionDigits = 0;
        bool hasDot = false;
        if (index < value.size() && value[index] == '.') {
            hasDot = true;
            ++index;
            const auto fractionStart = index;
            while (index < value.size() && value[index] >= '0' && value[index] <= '9')
                ++index;
            fractionDigits = index - fractionStart;
        }
        if ((integerDigits == 0 && fractionDigits == 0) || integerDigits > 18 ||
            fractionDigits > 12)
            return false;
        if (integerDigits == 0 && !hasDot)
            return false;
        if (index < value.size() && (value[index] == 'e' || value[index] == 'E')) {
            ++index;
            if (index < value.size() && (value[index] == '+' || value[index] == '-'))
                ++index;
            const auto exponentStart = index;
            while (index < value.size() && value[index] >= '0' && value[index] <= '9')
                ++index;
            if (index == exponentStart || index - exponentStart > 3)
                return false;
        }
        return index == value.size();
    }

    static bool decimalFieldValid(const ruvia::JsonValue& object, std::string_view field) {
        const auto raw = service::utils::jsonField(object, field);
        if (!raw)
            return true;
        const auto text = scalarText(*raw, true);
        if (!text || !decimalTokenValid(*text))
            return false;
        // Compare the exact decimal token so 1000000000.000000000001 does
        // not round down to the inclusive 1e9 limit.
        return compareNumeric(*text, "-1000000000") >= 0 &&
               compareNumeric(*text, "1000000000") <= 0;
    }

    template <typename Visitor>
    static bool visitArray(const ruvia::JsonValue& value, Visitor&& visitor) {
        if (!value.isArray())
            return false;
        auto input = value.view();
        ruvia::detail::skipJsonWhitespace(input);
        if (input.empty() || input.front() != '[')
            return false;
        input.remove_prefix(1);
        ruvia::detail::skipJsonWhitespace(input);
        if (!input.empty() && input.front() == ']') {
            input.remove_prefix(1);
            return input.empty();
        }
        for (;;) {
            const auto start = input;
            if (!ruvia::detail::skipJsonValue(input))
                return false;
            const auto element = start.substr(0, start.size() - input.size());
            const auto parsed = ruvia::JsonValue::parse(element);
            if (!parsed || !visitor(*parsed))
                return false;
            ruvia::detail::skipJsonWhitespace(input);
            if (!input.empty() && input.front() == ']') {
                input.remove_prefix(1);
                return input.empty();
            }
            if (!ruvia::detail::consumeJsonChar(input, ','))
                return false;
        }
    }

  public:
    static void validateConfig(const ruvia::JsonValue& payload, const std::string& protocol,
                               bool required) {
        if (!booleanFieldValid(payload, "enabled"))
            service::common::fail(16004, "enabled 必须是布尔值", 400);
        const auto config = service::utils::jsonField(payload, "config");
        if (!config) {
            if (required)
                service::common::fail(16004, "config 不能为空", 400);
            return;
        }
        if (!config->isObject())
            service::common::fail(16004, "config 必须是对象", 400);
        if (hasField(*config, "storageInterval"))
            service::common::fail(16004, "配置不允许 storageInterval，请使用 storagePolicy", 400);
        if (!enumFieldValid(*config, "storagePolicy", {"report", "change"}))
            service::common::fail(16004, "配置的 storagePolicy 无效", 400);
        if (required && !hasField(*config, "storagePolicy"))
            service::common::fail(16004, "配置的 storagePolicy 不能为空", 400);

        if (protocol == "SL651") {
            if (!enumFieldValid(*config, "responseMode", {"M1", "M2", "M3", "M4"}) ||
                (hasField(*config, "funcs") &&
                 !service::utils::jsonField(*config, "funcs")->isArray()) ||
                (required && (!hasField(*config, "responseMode") || !hasField(*config, "funcs"))))
                service::common::fail(16004, "SL651 配置无效", 400);
            const auto funcs = service::utils::jsonField(*config, "funcs");
            if (funcs && !visitArray(*funcs, [](const ruvia::JsonValue& function) {
                    const auto code = stringField(function, "funcCode");
                    if (!code || code->size() != 2 || !std::all_of(code->begin(), code->end(),
                        [](unsigned char ch) { return std::isxdigit(ch) != 0; }) ||
                        !enumFieldValid(function, "dir", {"UP", "DOWN"})) return false;
                    for (const auto field : {"elements", "responseElements"}) {
                        const auto elements = service::utils::jsonField(function, field);
                        if (!elements) continue;
                        if (!visitArray(*elements, [](const ruvia::JsonValue& element) {
                            if (!enumFieldValid(element, "positionMode", {"GUIDE", "OFFSET"})) return false;
                            const auto position = stringField(element, "positionMode").value_or("GUIDE");
                            if (position == "OFFSET") {
                                const auto offset = hasField(element, "byteOffset")
                                    ? integerFieldValue(element, "byteOffset", false, 7, 0, 8388607)
                                    : std::optional<std::int64_t>{0};
                                const auto length = integerFieldValue(element, "length", false, 7, 1, 8388608);
                                return offset && length && *offset + *length <= 8388608 &&
                                    integerFieldValid(element, "digits", false, 1, 0, 7, true) &&
                                    enumFieldValid(element, "encode", {"BCD", "HEX", "DICT", "JPEG", "TIME_YYMMDDHHMMSS"}) &&
                                    hasField(element, "encode");
                            }
                            if (position != "GUIDE") return false;
                            const auto guide = stringField(element, "guideHex");
                            const auto encoding = stringField(element, "encode");
                            if (!guide || (guide->size() != 4 && guide->size() != 6) ||
                                !std::all_of(guide->begin(), guide->end(),
                                    [](unsigned char ch) { return std::isxdigit(ch) != 0; }) ||
                                !encoding || !oneOf(*encoding, {"BCD", "HEX", "DICT", "JPEG", "TIME_YYMMDDHHMMSS"}) ||
                                !integerFieldValid(element, "length", false, 7, 0, 8388608, true) ||
                                !integerFieldValid(element, "digits", false, 1, 0, 7, true)) return false;
                            const auto lead = std::stoi(guide->substr(0, 2), nullptr, 16);
                            if ((lead == 0xFF) != (guide->size() == 6)) return false;
                            const auto definition = std::stoi(guide->substr(guide->size() - 2), nullptr, 16);
                            const auto length = std::stoi(*scalarText(*service::utils::jsonField(element, "length"), false));
                            const auto digits = std::stoi(*scalarText(*service::utils::jsonField(element, "digits"), false));
                            if (lead < 0xF0 || lead == 0xFF)
                                return length > 0 && length == (definition >> 3) &&
                                    (*encoding != "BCD" || digits == (definition & 7));
                            if (definition != lead) return false;
                            if (lead == 0xF0) return length == 5;
                            if (lead == 0xF1) return length == 6;
                            return lead == 0xF2 || lead == 0xF3 || length > 0;
                        })) return false;
                    }
                    return true;
                }))
                service::common::fail(16004, "SL651 引导符、长度或小数位不符合数据定义", 400);
            return;
        }

        if (protocol == "S7") {
            const auto connection = service::utils::jsonField(*config, "connection");
            const auto areas = service::utils::jsonField(*config, "areas");
            if (!enumFieldValid(*config, "plcModel",
                                {"S7-200", "S7-300", "S7-400", "S7-1200", "S7-1500"}) ||
                (connection && !connection->isObject()) || (areas && !areas->isArray()) ||
                (required && (!hasField(*config, "plcModel") || !connection || !areas)))
                service::common::fail(16004, "S7 配置无效", 400);
            if (connection &&
                (!enumFieldValid(*connection, "probeMode", {"STANDARD", "COMPATIBLE", "AUTO"}) ||
                 !numberFieldValid(*connection, "handshakeTimeout", false, "1000", "30000") ||
                 !numberFieldValid(*connection, "directProbeTimeout", false, "1000", "30000")))
                service::common::fail(16004, "S7 配置无效", 400);
            if (hasField(*config, "pollInterval"))
                service::common::fail(16004, "S7 配置不允许 pollInterval，请使用 readInterval", 400);
            if (!integerFieldValid(*config, "readInterval", true, 5, 1, 3600))
                service::common::fail(16004, "S7 配置的 readInterval 无效", 400);
            if (areas && !visitArray(*areas, [](const ruvia::JsonValue& area) {
                    if (!area.isObject())
                        return false;
                    const auto id = stringField(area, "id");
                    const auto name = stringField(area, "name");
                    const auto areaType = stringField(area, "area");
                    if (!id || id->empty() || !name || name->empty() || !areaType ||
                        !oneOf(*areaType, {"DB", "V", "MK", "PE", "PA", "CT", "TM"}))
                        return false;
                    if (!enumFieldValid(area, "dataType",
                                        {"BOOL", "INT8", "UINT8", "INT16", "UINT16", "INT32",
                                         "UINT32", "FLOAT", "LREAL", "STRING"}) ||
                        !integerFieldValid(area, "start", false, 10, 0, 2147483647, true) ||
                        !integerFieldValid(area, "size", false, 5, 1, 65535, true) ||
                        !integerFieldValid(area, "startBit", false, 1, 0, 7) ||
                        !signedIntegerFieldValid(area, "decimals", false, 2, -1, 8) ||
                        !booleanFieldValid(area, "writable"))
                        return false;
                    return *areaType != "DB" ||
                           integerFieldValid(area, "dbNumber", false, 5, 1, 65535, true);
                }))
                service::common::fail(16004, "S7 寄存器配置无效", 400);
            return;
        }

        if (protocol != "Modbus")
            return;
        const auto packet = service::utils::jsonField(*config, "packet");
        const auto registers = service::utils::jsonField(*config, "registers");
        if (!enumFieldValid(*config, "byteOrder",
                            {"BIG_ENDIAN", "LITTLE_ENDIAN", "BIG_ENDIAN_BYTE_SWAP",
                             "LITTLE_ENDIAN_BYTE_SWAP"}) ||
            (required && !hasField(*config, "byteOrder")))
            service::common::fail(16004, "Modbus 配置的 byteOrder 无效", 400);
        if ((registers && !registers->isArray()) || (required && !registers))
            service::common::fail(16004, "Modbus 配置的 registers 必须是数组", 400);
        if (packet && !packet->isObject())
            service::common::fail(16004, "Modbus 配置的 packet 必须是对象", 400);
        if (!integerFieldValid(*config, "readInterval", true, 5, 1, 3600))
            service::common::fail(16004, "Modbus 配置的 readInterval 无效", 400);
        if (packet && !integerFieldValid(*packet, "mergeGap", true, 5, 0, 2000))
            service::common::fail(16004, "Modbus 配置的 packet.mergeGap 无效", 400);
        if (packet && !integerFieldValid(*packet, "maxQuantity", true, 5, 1, 125))
            service::common::fail(16004, "Modbus 配置的 packet.maxQuantity 无效", 400);
        if (hasField(*config, "pollInterval"))
            service::common::fail(16004, "Modbus 配置不允许 pollInterval，请使用 readInterval", 400);
        if (!registers)
            return;
        if (!visitArray(*registers, [](const ruvia::JsonValue& entry) {
                if (!entry.isObject())
                    return false;
                const auto id = stringField(entry, "id");
                const auto name = stringField(entry, "name");
                const auto registerType = stringField(entry, "registerType");
                const auto dataType = stringField(entry, "dataType");
                if (!id || id->empty() || !name || name->empty() || !registerType || !dataType ||
                    !oneOf(*registerType,
                           {"COIL", "DISCRETE_INPUT", "HOLDING_REGISTER", "INPUT_REGISTER"}) ||
                    !oneOf(*dataType, {"BOOL", "INT16", "UINT16", "INT32", "UINT32", "FLOAT32",
                                       "INT64", "UINT64", "DOUBLE"}))
                    return false;
                return integerFieldValid(entry, "address", false, 5, 0, 65535, true) &&
                       integerFieldValid(entry, "quantity", false, 1, 1, 4, true);
            })) {
            service::common::fail(16004, "Modbus 寄存器配置无效", 400);
        }
        if (!visitArray(*registers, [](const ruvia::JsonValue& entry) {
                return entry.isObject() &&
                       enumFieldValid(entry, "byteOrder",
                                      {"BIG_ENDIAN", "LITTLE_ENDIAN", "BIG_ENDIAN_BYTE_SWAP",
                                       "LITTLE_ENDIAN_BYTE_SWAP"});
            }))
            service::common::fail(16004, "Modbus 寄存器 byteOrder 无效", 400);
        if (!visitArray(*registers, [](const ruvia::JsonValue& entry) {
                return entry.isObject() && decimalFieldValid(entry, "scale");
            }))
            service::common::fail(16004, "Modbus 寄存器 scale 无效", 400);
        if (!visitArray(*registers, [](const ruvia::JsonValue& entry) {
                return entry.isObject() &&
                       signedIntegerFieldValid(entry, "decimals", true, 2, -1, 8);
            }))
            service::common::fail(16004, "Modbus 寄存器 decimals 无效", 400);
        if (!visitArray(*registers, [](const ruvia::JsonValue& entry) {
                return entry.isObject() && booleanFieldValid(entry, "writable");
            }))
            service::common::fail(16004, "Modbus 寄存器 writable 必须是布尔值", 400);
    }

};

} // namespace service::protocol
