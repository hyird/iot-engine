#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "service/common/uuid.h"
#include "service/utils/expression.h"
#include "service/utils/json.h"

namespace service::common {

struct DerivedPoint {
    struct UnitRule {
        std::string condition;
        std::string unit;
    };

    std::string id;
    std::string name;
    std::string unit;
    std::string unitMode{ "fixed" };
    std::vector<UnitRule> unitRules;
    std::string sourceAlias;
    std::string kind;
    std::string expression;
    std::string valueType;
    std::vector<std::pair<std::string, std::string>> inputs;
    std::int64_t windowSeconds{};
    std::int64_t maxAgeSeconds{ 300 };
    bool visible{ true };
};

// Shared configuration contract; no runtime state or I/O.
inline std::vector<DerivedPoint> decodeDerivedPoints(const ruvia::JsonValue& config) {
    const auto raw = utils::jsonField(config, "derivedPoints");
    if (!raw) {
        return {};
    }
    std::vector<DerivedPoint> points;
    const auto text = [](const ruvia::JsonValue& object, std::string_view key, std::size_t maximum, bool required = true) {
        const auto value = object.get<ruvia::String>(key);
        if ((!value && (required || utils::jsonField(object, key))) ||
            (value && (value->view().size() > maximum || (required && value->view().empty())))) {
            throw std::invalid_argument("派生点字段无效: " + std::string(key));
        }
        return value ? std::string(value->view()) : std::string{};
    };
    const auto integer = [](const ruvia::JsonValue& object, std::string_view key, std::int64_t fallback, std::int64_t minimum) {
        if (!utils::jsonField(object, key)) {
            return fallback;
        }
        const auto value = object.get<ruvia::Int64>(key);
        if (!value || value->value < minimum || value->value > 86400) {
            throw std::invalid_argument("派生点时间参数无效: " + std::string(key));
        }
        return value->value;
    };
    std::set<std::string> ids;
    if (!utils::visitJsonArray(*raw, [&](const auto& item) {
            if (!item.isObject() || points.size() == 32) {
                throw std::invalid_argument("派生点最多 32 个");
            }
            DerivedPoint point;
            point.id = text(item, "id", 36);
            if (!isUuid(point.id) || !ids.insert(point.id).second) {
                throw std::invalid_argument("派生点标识无效或重复");
            }
            point.name = text(item, "name", 100);
            point.unit = text(item, "unit", 32, false);
            if (utils::jsonField(item, "unitMode")) {
                point.unitMode = text(item, "unitMode", 16);
            }
            if (point.unitMode != "fixed" && point.unitMode != "conditional") {
                throw std::invalid_argument("单位模式无效");
            }
            point.kind = text(item, "kind", 16);
            point.valueType = text(item, "valueType", 8);
            if (point.valueType != "number" && point.valueType != "boolean") {
                throw std::invalid_argument("派生点结果类型无效");
            }
            if (point.kind != "expression" && point.kind != "average" && point.kind != "minimum" && point.kind != "maximum") {
                throw std::invalid_argument("派生点计算类型无效");
            }
            point.expression = text(item, "expression", 512, point.kind == "expression");
            point.windowSeconds = integer(item, "windowSeconds", 0, 1);
            point.maxAgeSeconds = integer(item, "maxAgeSeconds", 300, 1);
            if (const auto visible = utils::jsonField(item, "visible")) {
                const auto value = item.template get<ruvia::Bool>("visible");
                if (!value) {
                    throw std::invalid_argument("visible 必须是布尔值");
                }
                point.visible = static_cast<bool>(*value);
            }
            if (point.kind != "expression" && (!point.windowSeconds || point.valueType != "number")) {
                throw std::invalid_argument("窗口派生点需要时间窗口和数值结果类型");
            }
            const auto inputs = utils::jsonField(item, "inputs");
            std::set<std::string> names;
            if (!inputs || !utils::visitJsonArray(*inputs, [&](const auto& input) {
                    const auto alias = text(input, "alias", 32);
                    const auto id = text(input, "pointId", 36);
                    const utils::Expression variable(alias);
                    if (variable.variables() != std::vector<std::string>{ alias } || !isUuid(id) || !names.insert(alias).second || point.inputs.size() == 16) {
                        throw std::invalid_argument("派生点输入绑定无效");
                    }
                    point.inputs.emplace_back(alias, id);
                    return true;
                }) ||
                point.inputs.empty()) {
                throw std::invalid_argument("派生点必须绑定输入点");
            }
            if (point.kind == "expression") {
                const utils::Expression expression(point.expression);
                for (const auto& name : expression.variables()) {
                    if (!names.contains(name)) {
                        throw std::invalid_argument("公式包含未绑定变量: " + name);
                    }
                }
            } else {
                point.sourceAlias = text(item, "sourceAlias", 32);
                if (!names.contains(point.sourceAlias)) {
                    throw std::invalid_argument("窗口计算的输入点未绑定");
                }
            }
            if (const auto rules = utils::jsonField(item, "unitRules")) {
                if (!utils::visitJsonArray(*rules, [&](const auto& rule) {
                        if (point.unitRules.size() == 8) {
                            throw std::invalid_argument("单位条件最多 8 条");
                        }
                        auto condition = text(rule, "condition", 512);
                        auto unit = text(rule, "unit", 32, false);
                        const utils::Expression expression(condition);
                        for (const auto& name : expression.variables()) {
                            if (!names.contains(name)) {
                                throw std::invalid_argument("单位条件包含未绑定变量: " + name);
                            }
                        }
                        point.unitRules.push_back({ std::move(condition), std::move(unit) });
                        return true;
                    })) {
                    throw std::invalid_argument("单位条件必须是数组");
                }
            }
            if ((point.unitMode == "conditional") != !point.unitRules.empty()) {
                throw std::invalid_argument("条件单位必须设置规则，固定单位不得保留条件");
            }
            points.push_back(std::move(point));
            return true;
        })) {
        throw std::invalid_argument("derivedPoints 必须是数组");
    }
    return points;
}

inline std::vector<DerivedPoint> orderDerivedPoints(const ruvia::JsonValue& config) {
    auto points = decodeDerivedPoints(config);
    std::set<std::string> physical;
    const auto collect = [&](const ruvia::JsonValue& array) {
        utils::visitJsonArray(array, [&](const auto& value) {
            if (const auto id = value.template get<ruvia::String>("id")) {
                physical.emplace(id->view());
            }
            return true;
        });
    };
    for (auto name : { "registers", "areas", "points" }) {
        if (auto array = utils::jsonField(config, name)) {
            collect(*array);
        }
    }
    if (const auto functions = utils::jsonField(config, "funcs")) {
        utils::visitJsonArray(*functions, [&](const auto& function) {
            // Only sampled upstream points can be formula inputs.
            const auto direction = function.template get<ruvia::String>("dir");
            if (direction && direction->view() == "UP") {
                if (auto elements = utils::jsonField(function, "elements")) {
                    collect(*elements);
                }
            }
            return true;
        });
    }
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (physical.contains(points[i].id)) {
            throw std::invalid_argument("派生点标识不能与采集点重复");
        }
        index.emplace(points[i].id, i);
    }
    std::vector<DerivedPoint> ordered;
    std::vector<unsigned> visited(points.size());
    const auto visit = [&](auto&& self, std::size_t i) -> void {
        if (visited[i] == 2) {
            return;
        }
        if (visited[i] == 1) {
            throw std::invalid_argument("派生点存在循环依赖");
        }
        visited[i] = 1;
        for (const auto& [alias, id] : points[i].inputs) {
            const auto found = index.find(id);
            if (found != index.end()) {
                self(self, found->second);
            } else if (!physical.contains(id)) {
                throw std::invalid_argument("派生点引用的采集点不存在或不是上行点");
            }
        }
        visited[i] = 2;
        ordered.push_back(points[i]);
    };
    for (std::size_t i = 0; i < points.size(); ++i) {
        visit(visit, i);
    }
    if (const auto visibility = utils::jsonField(config, "pointVisibility")) {
        if (!visibility->isObject() || !utils::visitJsonFields(*visibility, [&](std::string_view id, std::string_view raw) {
                return isUuid(id) && (raw == "true" || raw == "false");
            })) {
            throw std::invalid_argument("点位显示设置必须是标识到布尔值的映射");
        }
    }
    return ordered;
}
} // namespace service::common
