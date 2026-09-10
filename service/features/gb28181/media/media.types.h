#pragma once

#include <cstdint>
#include <string>

#include <ruvia/web/Model.h>

struct StreamStatus {
    std::string app;
    std::string stream;
    std::string schema;
    bool online{false};
    int readerCount{0};

    static std::string identity(const std::string& app, const std::string& stream,
                                const std::string& schema) {
        return app + ":" + schema + ":" + stream;
    }
};

struct PlayUrls {
    std::string httpFlv;
    std::string wsFlv;
    std::string httpTs;
    std::string hls;
    std::string webRtc;
    std::string rtsp;
    std::string rtmp;
};

struct OpenRtpServerResult {
    std::string streamId;
    uint16_t port{0};
    PlayUrls playUrls;
};

RUVIA_RESPONSE_MODEL(WebRtcResponseModel,
    RUVIA_OPTIONAL_FIELD(code, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
    RUVIA_OPTIONAL_FIELD(sdp, ruvia::String),
    RUVIA_OPTIONAL_FIELD(msg, ruvia::String));
