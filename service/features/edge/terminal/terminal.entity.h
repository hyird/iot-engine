#pragma once

#include <string>
#include <string_view>

namespace service::edge::terminal_state {

// 一次性票据以原始 Redis String 保存节点 ID，GETDEL 保证只能消费一次。
// Redis ORM 的键前缀和 Hash 字段布局不能映射现有 String 存储。
struct TerminalTicketRecord final {
    std::string nodeId;

    static std::string ticketKey(std::string_view ticket) {
        return "iot:edge:terminal:ticket:" + std::string(ticket);
    }
};

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

} // namespace service::edge::terminal_state
