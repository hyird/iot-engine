#pragma once

#include <cstdint>
#include <string>

struct SipConfig {
    std::string domain;
    std::string id;
    std::string host{"0.0.0.0"};
    std::string publicIp;
    std::uint16_t port{5060};
    std::string password;
    std::string transport{"udp"};
    bool logging{true};
};

struct MediaConfig {
    std::string zlmBaseUrl;
    std::string zlmPublicBaseUrl;
    std::string zlmSecret;
    std::string rtpPublicIp;
    std::uint16_t rtpPortRangeStart{30000};
    std::uint16_t rtpPortRangeEnd{30500};
};

struct AppConfig {
    bool enabled{false};
    SipConfig sip;
    MediaConfig media;
};
