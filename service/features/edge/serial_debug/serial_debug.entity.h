#pragma once
#include <optional>
#include <string>
#include <string_view>
#include "service/common/uuid.h"

namespace service::edge::serial_debug {
// 一次性 Redis String 票据必须以 GETDEL 消费；ORM Hash 不支持此存储布局。
struct TicketRecord final {
    std::string nodeId;
    std::string nodeSession;
    std::string path;
    static std::optional<TicketRecord> decode(std::string_view value) {
        const auto first = value.find('\n');
        const auto second = first == value.npos ? value.npos : value.find('\n', first + 1);
        if (first == value.npos || second == value.npos) return std::nullopt;
        TicketRecord record{std::string(value.substr(0, first)),
            std::string(value.substr(first + 1, second - first - 1)), std::string(value.substr(second + 1))};
        if (!service::common::isUuid(record.nodeId) || !record.path.starts_with("/dev/") ||
            record.path.size() > 96 || record.path.find_first_of("\r\n") != std::string::npos)
            return std::nullopt;
        return record;
    }
};

// 所有者与节点完整连接身份绑定，Lua 比较后续期和释放，不能用 ORM 非原子更新替代。
struct SessionRecord final {
    std::string nodeSession;
    static std::string key(std::string_view nodeId, std::string_view sessionId) {
        return "iot:edge:serial:session:" + std::string(nodeId) + ":" + std::string(sessionId);
    }
};
} // namespace service::edge::serial_debug
