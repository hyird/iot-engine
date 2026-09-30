#include <iostream>

#include "service/modules/protocol/protocol.service.h"

namespace {
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}

constexpr std::string_view kPoint = "00000000-0000-7000-8000-000000000001";

service::protocol::MqttPreviewResult preview(std::string_view config, std::string_view topic, std::string_view payload, std::string_view code = "") {
    const auto raw = "{\"config\":" + std::string(config) + ",\"topic\":" + service::utils::jsonQuoted(topic) + ",\"payload\":" + service::utils::jsonQuoted(payload) + ",\"deviceCode\":" + service::utils::jsonQuoted(code) + ",\"timezone\":\"+08:00\"}";
    const auto body = ruvia::fromJson<service::protocol::MqttPreviewBody>(raw);
    require(body.has_value(), "preview body did not parse");
    return service::protocol::ProtocolService::previewMqtt(*body);
}

std::string configuration(std::string_view options, std::string_view pointOptions, std::string_view field = "temperature", std::string_view type = "DOUBLE") {
    return "{\"storagePolicy\":\"report\",\"topic\":\"devices\",\"qos\":1," + std::string(options) + "\"points\":[{\"id\":\"" + std::string(kPoint) + "\",\"name\":\"温度\",\"field\":" + service::utils::jsonQuoted(field) + ",\"dataType\":" + service::utils::jsonQuoted(type) + std::string(pointOptions) + "}]}";
}

void testFormatsAndTime() {
    const auto json = configuration(R"("recordsPath":"/devices","identitySource":"payload","deviceCodeField":"/code","timeField":"/time","timeFormat":"iso8601",)", R"(,"scale":0.1,"offset":-1)");
    const auto result = preview(json, "devices", R"({"devices":[{"code":"D001","temperature":256,"time":"2026-01-01T08:00:00.123"},{"code":"D002","temperature":"bad"}]})");
    require(result.get<"records">().size() == 2, "nested records lost");
    const auto& record = result.get<"records">()[0];
    require(record.get<"deviceCode">().view() == "D001" && record.get<"time">().view() == "2026-01-01T00:00:00Z", "identity or timezone incorrect");
    require(std::abs(std::stod(std::string(record.get<"points">()[0].get<"value">().view())) - 24.6) < 0.00001, "scale and offset incorrect");
    require(result.get<"records">()[1].get<"errors">().size() == 1, "missing point diagnostic");
    const auto text = configuration(R"("payloadFormat":"text","identitySource":"payload","deviceCodeField":"0","delimiter":",","recordDelimiter":";",)", "", "1");
    const auto rows = preview(text, "devices", "D001,12.5;D002,18");
    require(rows.get<"records">().size() == 2 && rows.get<"records">()[1].get<"points">()[0].get<"value">().view() == "18", "text records or conversion failed");
    const auto binary = configuration(R"("payloadFormat":"binary","identitySource":"payload","deviceCodeField":"0:4:UTF8","recordLength":6,)", "", "4:2:UINT:LE");
    const auto bytes = preview(binary, "devices", "44 30 30 31 00 01 44 30 30 32 10 00");
    require(bytes.get<"records">().size() == 2 && bytes.get<"records">()[0].get<"points">()[0].get<"value">().view() == "256", "binary batch or byte order failed");
    const auto enums = configuration("", R"(,"enumValues":[{"input":"on","output":"true"},{"input":"off","output":"false"}])", "temperature", "BOOL");
    const auto enumResult = preview(enums, "devices", R"({"temperature":"on"})", "D001");
    require(enumResult.get<"records">()[0].get<"points">()[0].get<"value">().view() == "true", "enum conversion failed");
    auto topic = configuration(R"("identitySource":"topic","topicDeviceSegment":1,)", "");
    topic.replace(topic.find("\"topic\":\"devices\""), 17, "\"topic\":\"devices/+/telemetry\"");
    const auto topicResult = preview(topic, "devices/D002/telemetry", R"({"temperature":8})");
    require(topicResult.get<"records">()[0].get<"deviceCode">().view() == "D002", "topic identity failed");
}

void testRejectedConfiguration() {
    for (const auto config : {
             configuration(R"("identitySource":"payload",)", ""),
             configuration(R"("payloadFormat":"binary",)", "", "0:8:UINT"),
             configuration(R"("payloadFormat":"text",)", "", "-1"),
             configuration(R"("commandTopic":"commands",)", R"(,"writable":true,"scale":0)"),
             configuration("", R"(,"enumValues":[{"input":"a","output":"1"},{"input":"b","output":"1.0"}])") }) {
        bool rejected = false;
        try {
            (void)preview(config, "devices", "{}");
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "invalid configuration accepted");
    }
    bool rejected = false;
    try {
        (void)preview(configuration("", ""), "different", "{}");
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "unmatched sample topic accepted");
    auto wildcard = configuration(R"("identitySource":"topic","topicDeviceSegment":1,)", "");
    wildcard.replace(wildcard.find("\"topic\":\"devices\""), 17, "\"topic\":\"devices/+\"");
    rejected = false;
    try {
        (void)preview(wildcard, "devices/+", R"({"temperature":1})");
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "wildcard sample topic accepted as a concrete publication topic");
}
} // namespace

int main() {
    try {
        testFormatsAndTime();
        testRejectedConfiguration();
        std::cout << "MQTT configuration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
