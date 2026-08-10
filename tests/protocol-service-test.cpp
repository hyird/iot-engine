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

} // namespace

int main() {
    try {
        const auto source = protocolSource();
        requireNoUnsafeParsing(source);
        requirePartialModbusUpdateValidation(source);
        requireStrictUpdateFieldTypes(source);
        std::cout << "protocol service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "protocol service test failed: " << error.what() << '\n';
        return 1;
    }
}
