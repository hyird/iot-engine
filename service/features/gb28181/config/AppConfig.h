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
    std::string zlmPublicBaseUrl;
    std::string rtpPublicIp;
    int workerThreads{0};
    int logLevel{2};
    std::uint16_t httpPort{8080};
    std::uint16_t rtspPort{8554};
    std::uint16_t rtmpPort{1935};
    std::uint16_t rtcPort{8000};
    std::uint16_t srtPort{9000};
    std::uint16_t rtpPortRangeStart{30000};
    std::uint16_t rtpPortRangeEnd{30500};
};

struct AppConfig {
    bool enabled{false};
    SipConfig sip;
    MediaConfig media;
};
