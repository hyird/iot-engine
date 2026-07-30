#pragma once

#include <cstdint>
#include <string>

#include <ruvia/web/Model.h>

struct PlayUrls {
    std::string httpFlv;
    std::string wsFlv;
    std::string httpTs;
    std::string hls;
    std::string webRtc;
    std::string rtsp;
};

struct OpenRtpServerResult {
    std::string streamId;
    uint16_t port{0};
    PlayUrls playUrls;
};

struct WebRtcResponseModel final {
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64);
    RUVIA_OPTIONAL_FIELD(type, ruvia::String);
    RUVIA_OPTIONAL_FIELD(sdp, ruvia::String);
    RUVIA_OPTIONAL_FIELD(msg, ruvia::String);
    RUVIA_MODEL(WebRtcResponseModel, code, type, sdp, msg);
};
