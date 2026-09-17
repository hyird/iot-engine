#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "service/common/uuid.h"

namespace service::edge::terminal_state {

// 所有者保存完整节点会话标识，必须逐字比较，不能只比较 epoch。
// 关联的确认和序号键由 service 与所有者原子更新；队列仍属于消息契约。
struct TerminalSessionRecord final {
    std::string nodeSession;

    static std::string terminalSessionKey(std::string_view nodeId, std::string_view terminalId) {
        return "iot:edge:terminal:session:" + std::string(nodeId) + ":" +
            std::string(terminalId);
    }

    static std::string terminalInputAckKey(std::string_view nodeId, std::string_view terminalId) {
        return "iot:edge:terminal:in-ack:" + std::string(nodeId) + ":" +
            std::string(terminalId);
    }

    static std::string terminalOutputSequenceKey(std::string_view nodeId, std::string_view terminalId) {
        return "iot:edge:terminal:out-seq:" + std::string(nodeId) + ":" +
            std::string(terminalId);
    }
};

// Connection ownership spans several String keys and atomic Lua updates;
// a Redis Hash ORM entity cannot express the conditional queue/lease mutation.
struct TerminalBrowserRecord final {
    std::string userId;
    std::string connectionId;
    std::string nodeId;
    std::string nodeSession;

    static std::string key(std::string_view id) { return "iot:edge:terminal:browser:" + std::string(id); }

    static std::string closedKey(std::string_view id) { return "iot:edge:terminal:closed:" + std::string(id); }

    static std::string readyKey(std::string_view id) { return "iot:edge:terminal:ready:" + std::string(id); }

    static std::string remoteClosedKey(std::string_view id) { return "iot:edge:terminal:remote-closed:" + std::string(id); }

    static std::string inputSequenceKey(std::string_view id) { return "iot:edge:terminal:in-seq:" + std::string(id); }

    static std::string outputAckKey(std::string_view id) { return "iot:edge:terminal:out-ack:" + std::string(id); }

    std::string encode() const { return userId + "\n" + connectionId + "\n" + nodeId + "\n" + nodeSession; }

    static std::optional<TerminalBrowserRecord> decode(std::string_view value) {
        TerminalBrowserRecord record;
        for (auto* field : { &record.userId, &record.connectionId, &record.nodeId }) {
            const auto separator = value.find('\n');
            if (separator == value.npos || !service::common::isUuid(value.substr(0, separator))) {
                return std::nullopt;
            }
            *field = value.substr(0, separator);
            value.remove_prefix(separator + 1);
        }
        if (value.empty() || value.find('\n') != value.npos) {
            return std::nullopt;
        }
        record.nodeSession = value;
        return record;
    }
};

} // namespace service::edge::terminal_state
