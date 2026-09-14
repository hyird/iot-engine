#pragma once

#include <algorithm>
#include <ruvia/web/ModelObject.h>
#include "service/common/message.h"
#include "service/utils/crypto.h"
namespace service::telemetry::contract {
template<class Visit> void fields(std::string_view raw, Visit visit) {
    if (!ruvia::detail::visitJsonObjectFields(ruvia::detail::ResolvedPmrResourceTag{},raw,
        std::pmr::get_default_resource(),visit)) throw std::runtime_error("Invalid telemetry object");
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

// 旧固件仅使用实际提供的字段进行适配。
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
        if(!first)points+=',';first=false;
        points+=service::utils::jsonQuoted(id)+":{";
        fields(raw,[&](std::string_view key,std::string_view data){
            if(key!="value_type" && key!="quality" && key!="sample_time_ms" && key!="received_time_ms")
                points+=service::utils::jsonQuoted(key)+":"+std::string(data)+",";
            return true;
        });
        points+="\"value_type\":"+service::utils::jsonQuoted(kind)+",\"quality\":"+service::utils::jsonQuoted(invalid?"invalid":scalar->isNull()?"missing":"good")+
            ",\"sample_time_ms\":"+std::to_string(input.observedAtMs)+
            ",\"received_time_ms\":"+std::to_string(input.occurredAtMs)+"}";
        return true;
    });
    points+='}';
    fields(root->view(),[&](std::string_view key,std::string_view value){
        if(key!="values" && key!="model" && key!="event_kind" && key!="schema_version")out+=service::utils::jsonQuoted(key)+":"+std::string(value)+",";
        return true;
    });
    input.eventKind=media?"image":"sample";
    out+="\"schema_version\":1,\"event_kind\":"+service::utils::jsonQuoted(input.eventKind)+",\"values\":"+points+",\"model\":";
    out+=input.modelId.empty()?"null":"{\"id\":"+service::utils::jsonQuoted(input.modelId)+"}";
    input.valuesJson=out+'}';
    // 重传有新的接收时间和连接，但设备、采样时间与完整报文身份不变。
    // 在分发前统一身份，使直连与 EdgeNode 接入共用持久化及消息幂等边界。
    if (input.protocol == "SL651" && !input.deviceId.empty() &&
        input.observedAtMs > 0 && !input.rawPayloads.empty() &&
        std::all_of(input.rawPayloads.begin(), input.rawPayloads.end(),
                    [](const auto& bytes) { return !bytes.empty(); })) {
        const auto identity = "sl651-report-v1:" + service::utils::jsonQuoted(input.deviceId) +
            ':' + std::to_string(input.observedAtMs) + ':' +
            message::rawPayloadsJson(input.rawPayloads);
        auto hash = service::utils::sha256(identity);
        hash[12] = '8';
        hash[16] = "89ab"[service::common::hexDigit(hash[16]) & 3];
        input.messageId = hash.substr(0, 8) + '-' + hash.substr(8, 4) + '-' +
            hash.substr(12, 4) + '-' + hash.substr(16, 4) + '-' + hash.substr(20, 12);
    }
}
}
