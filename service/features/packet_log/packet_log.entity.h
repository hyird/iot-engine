#pragma once
#include <string>
#include <string_view>

namespace service::packet_log {
// 有界 Redis Stream 的动态报文字段无法由固定 Hash ORM 映射。
struct DebugPacketStream {
    static std::string key(std::string_view scope, std::string_view id) {
        return "iot:debug:packets:" + std::string(scope) + ':' + std::string(id);
    }
};
}
