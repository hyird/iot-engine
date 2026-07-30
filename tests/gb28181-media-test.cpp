#include "service/features/gb28181/media/ZlmSdk.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

} // namespace

int main() {
    try {
        MediaConfig config;
        config.rtpPublicIp = "127.0.0.1";
        config.workerThreads = 1;
        config.logLevel = 4;
        config.httpPort = 0;
        config.rtspPort = 0;
        config.rtmpPort = 0;
        config.rtcPort = 0;
        config.srtPort = 0;
        config.rtpPortRangeStart = 0;
        config.rtpPortRangeEnd = 0;

        ZlmSdk sdk(config);
        sdk.start();
        const auto ports = sdk.ports();
        require(ports.http != 0, "embedded ZLM HTTP server did not start");
        require(ports.rtsp != 0, "embedded ZLM RTSP server did not start");
        require(ports.rtmp != 0, "embedded ZLM RTMP server did not start");
        require(ports.rtc != 0, "embedded ZLM RTC server did not start");
        require(ports.srt != 0, "embedded ZLM SRT server did not start");

        using MediaHandle =
            std::unique_ptr<std::remove_pointer_t<mk_media>,
                            decltype(&mk_media_release)>;
        MediaHandle pcmMedia(
            mk_media_create("__defaultVhost__", "test", "pcm-aac", 0, 0, 0),
            &mk_media_release);
        require(pcmMedia != nullptr, "embedded ZLM PCM media source was not created");
        require(mk_media_init_audio(pcmMedia.get(), MKCodecAAC, 8000, 1, 16) == 1,
                "embedded ZLM AAC track was not initialized");
        mk_media_init_complete(pcmMedia.get());
        std::array<std::int16_t, 1024> pcm{};
        bool aacProduced = false;
        for (std::uint64_t frame = 0; frame < 8 && !aacProduced; ++frame) {
            aacProduced =
                mk_media_input_pcm(
                    pcmMedia.get(), pcm.data(),
                    static_cast<int>(pcm.size() * sizeof(pcm[0])), frame * 128 + 1) == 1;
        }
        require(aacProduced,
                "embedded ZLM FAAC PCM input did not produce AAC");
        pcmMedia.reset();

        const auto opened =
            sdk.openRtpServer("34020000002000000001", "34020000001320000001",
                              "2000000001");
        require(opened.has_value(), "embedded ZLM RTP server did not open");
        require(opened->port != 0, "embedded ZLM RTP server returned port zero");
        require(opened->playUrls.httpFlv.find(std::to_string(ports.http)) !=
                    std::string::npos,
                "HTTP-FLV URL does not use the embedded HTTP server");
        require(opened->playUrls.rtsp.find(std::to_string(ports.rtsp)) !=
                    std::string::npos,
                "RTSP URL does not use the embedded RTSP server");
        require(opened->playUrls.webRtc.find("/index/api/webrtc") != std::string::npos,
                "WebRTC URL does not use the C SDK signaling callback");
        require(sdk.closeRtpServer(opened->streamId),
                "embedded ZLM RTP server did not close");

        sdk.stop();
        require(!sdk.started(), "embedded ZLM runtime did not stop");
        std::cout << "gb28181 media sdk tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
