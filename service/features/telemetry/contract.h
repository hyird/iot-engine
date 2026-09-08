#pragma once
#include <ruvia/web/ModelObject.h>
#include "service/common/message/contract.h"
namespace service::telemetry::contract {
inline std::string quote(std::string_view input) {
    std::string out="\"";
    constexpr char hex[]="0123456789abcdef";
    for (const unsigned char ch : input) {
        if (ch=='"' || ch=='\\') {out+='\\';out+=static_cast<char>(ch);}
        else if(ch<32) {out+="\\u00";out+=hex[ch>>4];out+=hex[ch&15];}
        else out+=static_cast<char>(ch);
    }
    return out+'"';
}
template<class Visit> void fields(std::string_view raw, Visit visit) {
    if (!ruvia::detail::visitJsonObjectFields(ruvia::detail::ResolvedPmrResourceTag{},raw,
        std::pmr::get_default_resource(),visit)) throw std::runtime_error("Invalid telemetry object");
}
inline std::string_view field(std::string_view raw,std::string_view key) {
    std::string_view found;
    fields(raw,[&](std::string_view name,std::string_view value){if(name==key)found=value;return true;});
    return found;
}
// Old firmware is adapted here using only fields it actually supplied. The
// normalized envelope never guesses a model revision from mutable device state.
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
        points+=quote(id)+":{";
        fields(raw,[&](std::string_view key,std::string_view data){
            if(key!="value_type" && key!="quality" && key!="sample_time_ms" && key!="received_time_ms")
                points+=quote(key)+":"+std::string(data)+",";
            return true;
        });
        points+="\"value_type\":"+quote(kind)+",\"quality\":"+quote(invalid?"invalid":scalar->isNull()?"missing":"good")+
            ",\"sample_time_ms\":"+std::to_string(input.observedAtMs)+
            ",\"received_time_ms\":"+std::to_string(input.occurredAtMs)+"}";
        return true;
    });
    points+='}';
    fields(root->view(),[&](std::string_view key,std::string_view value){
        if(key!="values" && key!="model" && key!="event_kind" && key!="schema_version")out+=quote(key)+":"+std::string(value)+",";
        return true;
    });
    input.eventKind=media?"image":"sample";
    out+="\"schema_version\":1,\"event_kind\":"+quote(input.eventKind)+",\"values\":"+points+",\"model\":";
    out+=input.modelId.empty()?"null":"{\"id\":"+quote(input.modelId)+",\"revision\":"+std::to_string(input.modelRevision)+"}";
    input.valuesJson=out+'}';
}
}
