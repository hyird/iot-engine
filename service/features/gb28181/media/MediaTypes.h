#pragma once

#include <cstdint>
#include <string>

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
