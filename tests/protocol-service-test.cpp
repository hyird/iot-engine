#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string protocolSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/domains/protocol/protocol.service.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open protocol service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeParsing(std::string_view source) {
    require(source.find("std::stoll") == std::string_view::npos,
            "protocol service uses unsafe/partial stoll parsing");
    require(source.find("COALESCE((value->>'enabled')::boolean") == std::string_view::npos,
            "protocol service directly casts create enabled");
    require(source.find("THEN (body.value->>'enabled')::boolean") == std::string_view::npos,
            "protocol service directly casts update enabled");
}

void requirePartialModbusUpdateValidation(std::string_view source) {
    require(source.find("COALESCE(value->>'byteOrder', '') IN") == std::string_view::npos,
            "protocol service rejects Modbus partial updates without byteOrder");
    require(source.find("COALESCE(jsonb_typeof(value->'registers') = 'array', FALSE)") ==
                std::string_view::npos,
            "protocol service rejects Modbus partial updates without registers");
    require(source.find("NOT (value ? 'byteOrder') OR value->>'byteOrder' IN") !=
                std::string_view::npos,
            "protocol service does not allow missing byteOrder on partial Modbus update");
    require(source.find("('BIG_ENDIAN', 'LITTLE_ENDIAN', 'BIG_ENDIAN_BYTE_SWAP', "
                        "'LITTLE_ENDIAN_BYTE_SWAP')),") != std::string_view::npos,
            "protocol service Modbus byteOrder validation SQL is missing its closing parenthesis");
    require(source.find("NOT (value ? 'registers') OR jsonb_typeof(value->'registers') = 'array'") !=
                std::string_view::npos,
            "protocol service does not allow missing registers on partial Modbus update");
}

void requireStrictUpdateFieldTypes(std::string_view source) {
    require(source.find("if (const auto requested = payload.get<ruvia::String>(\"protocol\");") ==
                std::string_view::npos,
            "protocol update ignores present non-string protocol fields");
    require(source.find("if (const auto name = payload.get<ruvia::String>(\"name\"))") ==
                std::string_view::npos,
            "protocol update writes present non-string name fields through SQL");
    require(source.find("protocol 必须是字符串") != std::string_view::npos,
            "protocol update does not reject non-string protocol fields");
    require(source.find("name 必须是字符串") != std::string_view::npos,
            "protocol update does not reject non-string name fields");
    require(source.find("remark 必须是字符串或 null") != std::string_view::npos,
            "protocol service does not reject non-string remark fields");
}

void requireModbusRuntimeFieldValidation(std::string_view source) {
    require(source.find("Modbus 配置的 readInterval 无效") != std::string_view::npos,
            "protocol service does not validate Modbus readInterval");
    require(source.find("Modbus 配置的 packet.mergeGap 无效") != std::string_view::npos,
            "protocol service does not validate Modbus packet mergeGap");
    require(source.find("Modbus 配置的 packet.maxQuantity 无效") != std::string_view::npos,
            "protocol service does not validate Modbus packet maxQuantity");
    require(source.find("Modbus 寄存器 scale 无效") != std::string_view::npos,
            "protocol service does not validate Modbus register scale");
    require(source.find("Modbus 寄存器 decimals 无效") != std::string_view::npos,
            "protocol service does not validate Modbus register decimals");
    require(source.find("Modbus 寄存器 writable 必须是布尔值") != std::string_view::npos,
            "protocol service does not validate Modbus register writable");
}

void requireUnifiedReadIntervalValidation(std::string_view source) {
    require(source.find("S7 配置的 readInterval 无效") != std::string_view::npos,
            "protocol service does not validate S7 readInterval");
    require(source.find("S7 配置不允许 pollInterval，请使用 readInterval") !=
                std::string_view::npos,
            "protocol service still accepts S7 pollInterval");
    require(source.find("Modbus 配置不允许 pollInterval，请使用 readInterval") !=
                std::string_view::npos,
            "protocol service still accepts Modbus pollInterval");
}

void requireUnifiedStoragePolicyValidation(std::string_view source) {
    require(source.find("配置不允许 storageInterval，请使用 storagePolicy") !=
                std::string_view::npos,
            "protocol service still accepts the retired storage interval");
    require(source.find("value->>'storagePolicy' IN ('report', 'change')") !=
                std::string_view::npos,
            "protocol service does not validate canonical storage policies");
    require(source.find("配置的 storagePolicy 不能为空") != std::string_view::npos,
            "protocol create does not require a storage policy");
}

void requireQualifiedJsonArrayValidation(std::string_view source) {
    require(source.find("jsonb_array_elements(value->'areas')") ==
                std::string_view::npos,
            "S7 validation leaves value ambiguous beside jsonb_array_elements");
    require(source.find("jsonb_array_elements(value->'registers')") ==
                std::string_view::npos,
            "Modbus validation leaves value ambiguous beside jsonb_array_elements");
    require(source.find(
                "jsonb_array_elements(cfg.value->'areas') AS area(item)") !=
                std::string_view::npos,
            "S7 validation does not qualify the config and array element columns");
    require(source.find(
                "jsonb_array_elements(cfg.value->'registers') AS entry(item)") !=
                std::string_view::npos,
            "Modbus validation does not qualify the config and array element columns");
}

void requireEdgeSyncDoesNotLeakAsUpdateFailure(std::string_view source) {
    require(source.find("l.edge_node_id IS NOT NULL") != std::string_view::npos,
            "protocol update can queue empty edge node IDs");
    require(source.find("protocol edge config sync failed") != std::string_view::npos,
            "protocol update does not isolate edge config sync failures");
    require(source.find("catch (const std::exception& error)") != std::string_view::npos,
            "protocol update does not catch edge config sync exceptions");
    require(source.find("enqueueConfigEvent(transaction, \"protocol\", \"updated\"") !=
                std::string_view::npos,
            "protocol update does not enqueue its event transactionally");
    require(source.find("publishConfigEvent") == std::string_view::npos,
            "protocol update still performs a database/Redis dual write");
    require(source.find("logPostUpdateFailure(\"edge-sync\"") != std::string_view::npos,
            "protocol update does not isolate edge sync query failures");
    require(source.find("protocol post-update ") != std::string_view::npos,
            "protocol update does not log post-update failures");
    require(source.find("catch (...)") != std::string_view::npos,
            "protocol update does not catch non-std post-update exceptions");
    require(source.find("error=unknown exception") != std::string_view::npos,
            "protocol update does not log non-std edge sync exceptions");
}

} // namespace

int main() {
    try {
        const auto source = protocolSource();
        requireNoUnsafeParsing(source);
        requirePartialModbusUpdateValidation(source);
        requireStrictUpdateFieldTypes(source);
        requireModbusRuntimeFieldValidation(source);
        requireUnifiedReadIntervalValidation(source);
        requireUnifiedStoragePolicyValidation(source);
        requireQualifiedJsonArrayValidation(source);
        requireEdgeSyncDoesNotLeakAsUpdateFailure(source);
        std::cout << "protocol service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "protocol service test failed: " << error.what() << '\n';
        return 1;
    }
}
