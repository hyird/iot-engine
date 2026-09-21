#pragma once

#include <algorithm>
#include <ruvia/web/ModelObject.h>
#include "service/common/message.h"
#include "service/utils/crypto.h"
#include "service/utils/json.h"
namespace service::telemetry::contract {
template<class Visit> void fields(std::string_view raw, Visit visit) {
    const auto object = ruvia::JsonValue::parse(raw);
    if (!object || !object->forEachField([&](std::string_view name, const ruvia::JsonValue& value) {
        return visit(name, value.view());
    })) throw std::runtime_error("Invalid telemetry object");
}
inline std::string_view field(std::string_view raw,std::string_view key) {
    std::string_view found;
    fields(raw,[&](std::string_view name,std::string_view value){if(name==key)found=value;return true;});
    return found;
}
// 无要素的 SL651 报文只表示设备活动，不能作为指标样本。
inline bool isSl651EmptyReport(const message::ParsedDeviceMessage& input) {
    if (input.protocol != "SL651") return false;
    const auto values = field(input.valuesJson, "values");
    bool empty = true;
    fields(values, [&](std::string_view, std::string_view) {
        empty = false;
        return true;
    });
    return empty;
}

// 将采集结果规范化为统一的历史值结构，保留采集端创建的轮次身份。
inline void normalize(message::ParsedDeviceMessage& input) {
    const auto root=ruvia::JsonValue::parse(input.valuesJson);
    if(!root || !root->isObject()) throw std::runtime_error("Telemetry must be a JSON object");
    const auto values=field(root->view(),"values");
    const auto parsedValues=ruvia::JsonValue::parse(values);
    if(!parsedValues || !parsedValues->isObject()) throw std::runtime_error("Telemetry values must be an object");
    std::string out="{",points="{";
    bool first=true,media=false;
    fields(values,[&](std::string_view id,std::string_view raw) {
        const auto point=ruvia::JsonValue::parse(raw);
        if(!point || !point->isObject()) throw std::runtime_error("Telemetry point must be an object");
        auto value=field(raw,"value");
        const auto scalar=ruvia::JsonValue::parse(value);
        if(!scalar) throw std::runtime_error("Telemetry point has no value");
        const auto encoding=point->get<ruvia::String>("type");
        const bool image=encoding && encoding->view()=="JPEG";
        media=media||image;
        const std::string kind=image?"media":scalar->isNumber()?"number":scalar->isBoolean()?"boolean":
            scalar->isString()?"string":scalar->isNull()?"null":"json";
        const auto text=point->get<ruvia::String>("value");
        const bool invalid=image && text && text->view()=="INVALID_JPEG";
        const auto derived = point->get<ruvia::Bool>("derived");
        const auto reportedQuality = point->get<ruvia::String>("quality");
        const auto quality = derived && static_cast<bool>(*derived) && reportedQuality ? std::string(reportedQuality->view()) : std::string(invalid?"invalid":scalar->isNull()?"missing":"good");
        const auto sampleTime = derived && static_cast<bool>(*derived) ? point->get<ruvia::Int64>("sample_time_ms") : std::nullopt;
        if(!first)points+=',';first=false;
        points+=service::utils::jsonQuoted(id)+":{";
        fields(raw,[&](std::string_view key,std::string_view data){
            if(key!="value_type" && key!="quality" && key!="sample_time_ms" && key!="received_time_ms")
                points+=service::utils::jsonQuoted(key)+":"+std::string(data)+",";
            return true;
        });
        points+="\"value_type\":"+service::utils::jsonQuoted(kind)+",\"quality\":"+service::utils::jsonQuoted(quality)+
            ",\"sample_time_ms\":"+std::to_string(sampleTime ? sampleTime->value : input.observedAtMs)+
            ",\"received_time_ms\":"+std::to_string(input.occurredAtMs)+"}";
        return true;
    });
    points+='}';
    fields(root->view(),[&](std::string_view key,std::string_view value){
        if(key!="values" && key!="model" && key!="event_kind" && key!="schema_version" && key!="raw_packet_ids")out+=service::utils::jsonQuoted(key)+":"+std::string(value)+",";
        return true;
    });
    input.eventKind=media?"image":"sample";
    out += "\"raw_packet_ids\":[";
    for (std::size_t index = 0; index < input.rawPacketIds.size(); ++index) {
        if (index) out += ',';
        out += service::utils::jsonQuoted(input.rawPacketIds[index]);
    }
    out += "],";
    out+="\"schema_version\":1,\"event_kind\":"+service::utils::jsonQuoted(input.eventKind)+",\"values\":"+points+",\"model\":";
    out+=input.modelId.empty()?"null":"{\"id\":"+service::utils::jsonQuoted(input.modelId)+"}";
    input.valuesJson=out+'}';
    // 历史身份由采集端创建；接收时间和内容不能重写轮次身份。
    if (input.acquisitionId.empty()) throw std::invalid_argument("telemetry acquisition ID is required");
    input.messageId = input.acquisitionId;
}
}
