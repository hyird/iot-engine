#include "service/common/log.h"
#include "service/common/packet-log.h"
#include "service/features/gb28181/media/MediaProxy.h"
#include "service/features/gb28181/media/ZlmSdk.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
        namespace packetLog = service::common::packet_log;
        packetLog::Config packetConfig;
        packetConfig.directory =
            std::filesystem::temp_directory_path() / "iot-engine-gb-packet-log";
        packetConfig.level = packetLog::Level::Info;
        packetLog::initialize(std::move(packetConfig));
        packetLog::shutdown();
        // Packet-log shutdown must not destroy spdlog's process-wide default
        // logger; GB startup logs immediately after this lifecycle transition.
        LOG_INFO << "packet logger lifecycle retained the application logger";

        require(service::gb28181::media_proxy_detail::allowedTarget(
                    "GET", "/rtp/stream.live.flv"),
                "media proxy rejected an HTTP-FLV target");
        require(service::gb28181::media_proxy_detail::allowedTarget(
                    "POST", "/index/api/webrtc"),
                "media proxy rejected WebRTC signaling");
        require(!service::gb28181::media_proxy_detail::allowedTarget(
                    "GET", "/index/api/getServerConfig"),
                "media proxy exposed an internal ZLM API");
        require(!service::gb28181::media_proxy_detail::allowedTarget(
                    "GET", "/rtp/../index/api/getServerConfig"),
                "media proxy accepted a traversal target");
        require(!service::gb28181::media_proxy_detail::allowedTarget(
                    "GET", "/rtp/%2e%2e/index/api/getServerConfig") &&
                    !service::gb28181::media_proxy_detail::allowedTarget(
                        "GET", "/rtp/..\\index\\api\\getServerConfig"),
                "media proxy accepted an encoded traversal target");
        require(service::gb28181::media_proxy_detail::rewriteHlsPlaylist(
                    "#EXTM3U\nsegment-1.ts\nsegment-2.ts?part=1\n",
                    "token=x&expires=1") == "#EXTM3U\nsegment-1.ts?token=x&expires=1\n"
                                            "segment-2.ts?part=1&token=x&expires=1\n",
                "HLS proxy did not preserve playback authorization on child requests");

        MediaConfig config;
        config.rtpPublicIp = "127.0.0.1";
        config.playTokenSecret = "gb28181-media-test-secret";
        config.workerThreads = 1;
        config.logLevel = 4;
        config.httpPort = 0;
        config.rtspPort = 0;
        config.rtmpPort = 0;
        config.rtcPort = 0;
        config.srtPort = 0;
        config.rtpPortRangeStart = 0;
        config.rtpPortRangeEnd = 0;

        std::atomic_int pcmRegistered{0};
        std::atomic_int pcmDeregistered{0};
        std::atomic_int invalidDeregisteredReaderCount{0};
        ZlmSdk::Callbacks callbacks;
        callbacks.onStreamChanged =
            [&pcmRegistered, &pcmDeregistered,
             &invalidDeregisteredReaderCount](const std::string&,
                                              const std::string& stream,
                                              const std::string&, bool online,
                                              int readerCount) {
                if (stream != "pcm-aac")
                    return;
                if (online) {
                    ++pcmRegistered;
                } else {
                    ++pcmDeregistered;
                    if (readerCount != 0)
                        ++invalidDeregisteredReaderCount;
                }
            };
        ZlmSdk sdk(config, std::move(callbacks));
        sdk.start();
        const auto ports = sdk.ports();
        require(ports.http != 0, "embedded ZLM HTTP server did not start");
        require(ports.rtsp != 0, "embedded ZLM RTSP server did not start");
        require(ports.rtmp != 0, "embedded ZLM RTMP server did not start");
        require(ports.rtc != 0, "embedded ZLM RTC server did not start");
        require(ports.srt != 0, "embedded ZLM SRT server did not start");

        using MediaHandle = std::unique_ptr<std::remove_pointer_t<mk_media>,
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
                mk_media_input_pcm(pcmMedia.get(), pcm.data(),
                                   static_cast<int>(pcm.size() * sizeof(pcm[0])),
                                   frame * 128 + 1) == 1;
        }
        require(aacProduced, "embedded ZLM FAAC PCM input did not produce AAC");
        require(pcmRegistered.load() > 0,
                "embedded ZLM media registration callback was not observed");
        pcmMedia.reset();
        require(pcmDeregistered.load() > 0 &&
                    invalidDeregisteredReaderCount.load() == 0,
                "embedded ZLM media deregistration queried an unsafe reader count");

        const auto opened = sdk.openRtpServer("34020000002000000001",
                                              "34020000001320000001", "2000000001");
        require(opened.has_value(), "embedded ZLM RTP server did not open");
        require(opened->port != 0, "embedded ZLM RTP server returned port zero");
        require(opened->port >= 30000 && opened->port <= 35000 && opened->port % 2 == 0,
                "automatic RTP allocation did not return a managed even port");
        require(opened->playUrls.httpFlv.find(std::to_string(ports.http)) !=
                    std::string::npos,
                "HTTP-FLV URL does not use the embedded HTTP server");
        require(opened->playUrls.rtsp.find(std::to_string(ports.rtsp)) !=
                    std::string::npos,
                "RTSP URL does not use the embedded RTSP server");
        require(opened->playUrls.rtmp.find(std::to_string(ports.rtmp)) !=
                    std::string::npos,
                "RTMP URL does not use the embedded RTMP server");
        require(opened->playUrls.webRtc.find("/index/api/webrtc") != std::string::npos,
                "WebRTC URL does not use the C SDK signaling callback");
        require(opened->playUrls.wsFlv.empty(),
                "media proxy advertised an unsupported WebSocket playback URL");
        const auto tokenAt = opened->playUrls.httpFlv.find("token=");
        const auto expiresAt = opened->playUrls.httpFlv.find("&expires=");
        require(tokenAt != std::string::npos && expiresAt != std::string::npos,
                "play URL is not signed");
        const auto token =
            opened->playUrls.httpFlv.substr(tokenAt + 6, expiresAt - (tokenAt + 6));
        const auto expires = std::stoll(opened->playUrls.httpFlv.substr(expiresAt + 9));
        require(sdk.validatePlayToken(opened->streamId, token, expires),
                "generated play token is not valid");
        require(!sdk.validatePlayToken(opened->streamId, token + "0", expires),
                "tampered play token was accepted");
        require(!sdk.validatePlayToken(opened->streamId, token, 1),
                "expired play token was accepted");
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
