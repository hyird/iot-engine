#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "service/common/uuid.h"

namespace service::edge::serial_debug {
// 目标是浏览器 Redis String 绑定记录的一部分，保留节点完整连接身份与串口路径。
struct SerialTargetRecord final {
    std::string nodeId;
    std::string nodeSession;
    std::string path;

    static std::optional<SerialTargetRecord> decode(std::string_view value) {
        const auto first = value.find('\n');
        const auto second = first == value.npos ? value.npos : value.find('\n', first + 1);
        if (first == value.npos || second == value.npos) {
            return std::nullopt;
        }
        SerialTargetRecord record{ std::string(value.substr(0, first)),
                                   std::string(value.substr(first + 1, second - first - 1)),
                                   std::string(value.substr(second + 1)) };
        if (!service::common::isUuid(record.nodeId) || !record.path.starts_with("/dev/") ||
            record.path.size() > 96 || record.path.find_first_of("\r\n") != std::string::npos) {
            return std::nullopt;
        }
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

// 浏览器会话绑定用户及实际 WS 连接；Redis String 与 Lua 条件释放不能用 Hash ORM 替代。
struct BrowserBindingRecord final {
    std::string userId;
    std::string connectionId;
    SerialTargetRecord ticket;

    static std::string key(std::string_view id) {
        return "iot:edge:serial:browser:" + std::string(id);
    }

    static std::string sequenceKey(std::string_view id) {
        return "iot:edge:serial:sequence:" + std::string(id);
    }

    static std::string closedKey(std::string_view id) {
        return "iot:edge:serial:closed:" + std::string(id);
    }

    std::string encode() const {
        return userId + "\n" + connectionId + "\n" + ticket.nodeId + "\n" + ticket.nodeSession + "\n" + ticket.path;
    }

    static std::optional<BrowserBindingRecord> decode(std::string_view value) {
        const auto first = value.find('\n');
        const auto second = first == value.npos ? value.npos : value.find('\n', first + 1);
        if (second == value.npos) {
            return std::nullopt;
        }
        auto ticket = SerialTargetRecord::decode(value.substr(second + 1));
        if (!ticket || !service::common::isUuid(value.substr(0, first)) ||
            !service::common::isUuid(value.substr(first + 1, second - first - 1))) {
            return std::nullopt;
        }
        return BrowserBindingRecord{ std::string(value.substr(0, first)),
                                     std::string(value.substr(first + 1, second - first - 1)),
                                     std::move(*ticket) };
    }
};
} // namespace service::edge::serial_debug
