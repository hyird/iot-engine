#pragma once
#include "native_services.h"
#include "../common/common.h"
namespace iotvpn::service_control::services {
inline Json encodeServiceSnapshot(const WindowsServiceSnapshot& s) {
    Json j;
#define FIELD(x) j[#x] = s.x
    FIELD(present); FIELD(running); FIELD(delayed); FIELD(failureNonCrash);
    FIELD(type); FIELD(startType); FIELD(errorControl); FIELD(sidType); FIELD(resetPeriod);
#undef FIELD
#define FIELD(x) j[#x] = utf8(s.x)
    FIELD(binary); FIELD(display); FIELD(account); FIELD(dependencies); FIELD(rebootMessage); FIELD(command);
#undef FIELD
    j["actions"] = Json::array();
    for (const auto& a : s.actions) j["actions"].push_back({static_cast<int>(a.Type), a.Delay});
    return j;
}
inline WindowsServiceSnapshot decodeServiceSnapshot(const Json& j) {
    WindowsServiceSnapshot s;
#define FIELD(x) j.at(#x).get_to(s.x)
    FIELD(present); FIELD(running); FIELD(delayed); FIELD(failureNonCrash);
    FIELD(type); FIELD(startType); FIELD(errorControl); FIELD(sidType); FIELD(resetPeriod);
#undef FIELD
#define FIELD(x) s.x = utf16(j.at(#x).get<std::string>())
    FIELD(binary); FIELD(display); FIELD(account); FIELD(dependencies); FIELD(rebootMessage); FIELD(command);
#undef FIELD
    for (const auto& a : j.at("actions")) s.actions.push_back({static_cast<SC_ACTION_TYPE>(a.at(0).get<int>()), a.at(1).get<DWORD>()});
    return s;
}
}
