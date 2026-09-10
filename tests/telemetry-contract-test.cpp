#include <iostream>
#include <stdexcept>
#include "service/features/telemetry/telemetry.protocol.h"

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        service::message::ParsedDeviceMessage input;
        input.observedAtMs=123;input.occurredAtMs=456;
        input.valuesJson=R"({"values":{"n":{"value":12.5,"unit":"C"},"b":{"value":true},"s":{"value":"0012"},"nil":{"value":null},"text":{"value":"\"type\":\"JPEG\""}}})";
        service::telemetry::contract::normalize(input);
        const auto decoded=ruvia::JsonValue::parse(input.valuesJson);
        require(decoded.has_value(),"normalized telemetry is invalid JSON");
        require(input.eventKind=="sample","text accidentally classified as media");
        require(input.valuesJson.find("\"model\":null")!=std::string::npos,"legacy model provenance was fabricated");
        require(input.valuesJson.find("\"value_type\":\"number\"")!=std::string::npos,"numeric type lost");
        require(input.valuesJson.find("\"value_type\":\"boolean\"")!=std::string::npos,"boolean type lost");
        require(input.valuesJson.find("\"value\":\"0012\"")!=std::string::npos,"string precision changed");
        require(input.valuesJson.find("\"quality\":\"missing\"")!=std::string::npos,"null quality not represented");
        require(input.valuesJson.find("\"sample_time_ms\":123")!=std::string::npos,"sample time changed");
        require(input.valuesJson.find("\"received_time_ms\":456")!=std::string::npos,"receipt time changed");
        const auto once=input.valuesJson;
        service::telemetry::contract::normalize(input);
        require(input.valuesJson==once,"normalization is not idempotent");
        input.modelId="00000000-0000-7000-8000-000000000002";input.modelRevision=7;
        input.valuesJson=R"({"values":{"image":{"type":"JPEG","value":"INVALID_JPEG"}}})";
        service::telemetry::contract::normalize(input);
        require(input.eventKind=="image","JPEG was not classified as media");
        require(input.valuesJson.find("\"quality\":\"invalid\"")!=std::string::npos,"invalid media quality lost");
        require(input.valuesJson.find("\"revision\":7")!=std::string::npos,"actual model revision lost");
        input.valuesJson=R"({"values":{"bad":{"unit":"C"}}})";
        bool rejected=false;
        try {service::telemetry::contract::normalize(input);}catch(const std::runtime_error&){rejected=true;}
        require(rejected,"missing values silently accepted");
        std::cout<<"telemetry contract tests passed\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
