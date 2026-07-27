import Hls from 'hls.js';
import mpegts from 'mpegts.js';
import { useEffect, useRef, useState } from 'react';
import { Endpoint, Events } from 'zlmrtc-client';
import {
    createJessibucaFlvPlayer,
    type JessibucaFlvPlayer,
} from '@/lib/gb28181/jessibucaFlvPlayer';
import type { GB28181 } from '../gb28181.types';

const WEBRTC_FALLBACK_DELAY_MS = 8000;
const WEBRTC_CODEC_RETRY_MS = 400;
const WEBRTC_CODEC_RETRY_COUNT = 10;

type PlaybackCandidate = {
    label: string;
    engine: 'jessibuca' | 'hls' | 'mpegts';
    mediaType?: 'flv' | 'mpegts';
    url: string;
};

type PlayerResources = {
    hls?: Hls;
    mpegts?: ReturnType<typeof mpegts.createPlayer>;
    rtc?: Endpoint;
    jessibuca?: JessibucaFlvPlayer;
    fallbackTimer?: number;
};

const resetVideoElement = (video: HTMLVideoElement) => {
    video.pause();
    video.removeAttribute('src');
    video.srcObject = null;
    video.load();
};

const closePlayerResources = (resources: PlayerResources, video?: HTMLVideoElement | null) => {
    if (resources.fallbackTimer) {
        window.clearTimeout(resources.fallbackTimer);
        resources.fallbackTimer = undefined;
    }
    resources.hls?.destroy();
    resources.hls = undefined;
    resources.mpegts?.destroy();
    resources.mpegts = undefined;
    resources.rtc?.close();
    resources.rtc = undefined;
    resources.jessibuca?.close();
    resources.jessibuca = undefined;
    if (video) resetVideoElement(video);
};

const buildPlaybackCandidates = (urls: GB28181.PlayUrls): PlaybackCandidate[] => {
    const candidates: Array<PlaybackCandidate | null> = [
        urls.http_flv
            ? { label: 'Jessibuca HTTP-FLV', engine: 'jessibuca', url: urls.http_flv }
            : null,
        urls.ws_flv ? { label: 'Jessibuca WS-FLV', engine: 'jessibuca', url: urls.ws_flv } : null,
        urls.hls ? { label: 'HLS', engine: 'hls', url: urls.hls } : null,
        urls.http_flv
            ? { label: 'HTTP-FLV', engine: 'mpegts', mediaType: 'flv', url: urls.http_flv }
            : null,
        urls.ws_flv
            ? { label: 'WS-FLV', engine: 'mpegts', mediaType: 'flv', url: urls.ws_flv }
            : null,
        urls.http_ts
            ? { label: 'HTTP-TS', engine: 'mpegts', mediaType: 'mpegts', url: urls.http_ts }
            : null,
    ];
    return candidates.filter((candidate): candidate is PlaybackCandidate =>
        Boolean(candidate?.url)
    );
};

const AUXILIARY_VIDEO_CODECS = new Set(['RTX', 'RED', 'ULPFEC', 'FLEXFEC-03']);

type RtcEndpointWithPeerConnection = Endpoint & {
    pc?: RTCPeerConnection | null;
};

type RtcStatsRecord = {
    id?: string;
    type?: string;
    kind?: string;
    mediaType?: string;
    codecId?: string;
    mimeType?: string;
    payloadType?: number;
    packetsReceived?: number;
    framesDecoded?: number;
};

const normalizeVideoCodecName = (value?: string) => {
    if (!value) return undefined;
    let codec = value.trim();
    const mimeMatch = /^video\/([^;\s]+)/i.exec(codec);
    if (mimeMatch?.[1]) {
        codec = mimeMatch[1];
    }
    codec = codec.split(';')[0].split('/')[0].trim().toUpperCase();
    if (!codec || AUXILIARY_VIDEO_CODECS.has(codec)) return undefined;
    if (codec === 'HEVC' || codec === 'HVC1') return 'H265';
    if (codec === 'AVC' || codec === 'AVC1') return 'H264';
    return codec;
};

const parseVideoCodecFromSdp = (sdp?: string) => {
    if (!sdp) return undefined;
    const lines = sdp.split(/\r?\n/).map((line) => line.trim());
    const videoLineIndex = lines.findIndex((line) => line.startsWith('m=video '));
    if (videoLineIndex < 0) return undefined;

    const payloadTypes = lines[videoLineIndex].split(/\s+/).slice(3);
    const payloadCodecs = new Map<string, string>();
    for (let index = videoLineIndex + 1; index < lines.length; index += 1) {
        const line = lines[index];
        if (line.startsWith('m=')) break;
        const match = /^a=rtpmap:(\d+)\s+([^\s]+)/i.exec(line);
        const codec = normalizeVideoCodecName(match?.[2]);
        if (match?.[1] && codec) payloadCodecs.set(match[1], codec);
    }

    for (const payloadType of payloadTypes) {
        const codec = payloadCodecs.get(payloadType);
        if (codec) return codec;
    }
    return undefined;
};

const getEndpointPeerConnection = (endpoint: Endpoint) =>
    (endpoint as RtcEndpointWithPeerConnection).pc ?? null;

const findWebRtcVideoCodecFromStats = async (pc: RTCPeerConnection) => {
    try {
        const report = await pc.getStats();
        const codecsById = new Map<string, string>();
        const codecsByPayloadType = new Map<number, string>();

        report.forEach((value: RtcStatsRecord) => {
            if (value.type !== 'codec') return;
            const codec = normalizeVideoCodecName(value.mimeType);
            if (!codec) return;
            if (value.id) codecsById.set(value.id, codec);
            if (typeof value.payloadType === 'number')
                codecsByPayloadType.set(value.payloadType, codec);
        });

        let selectedCodecId: string | undefined;
        let selectedPayloadType: number | undefined;
        let selectedScore = -1;
        report.forEach((value: RtcStatsRecord) => {
            if (value.type !== 'inbound-rtp') return;
            if (value.kind !== 'video' && value.mediaType !== 'video') return;
            const score = (value.framesDecoded ?? 0) + (value.packetsReceived ?? 0);
            if (score < selectedScore) return;
            selectedScore = score;
            selectedCodecId = value.codecId;
            selectedPayloadType = value.payloadType;
        });

        if (selectedCodecId) {
            const codec = codecsById.get(selectedCodecId);
            if (codec) return codec;
        }
        if (typeof selectedPayloadType === 'number') {
            return codecsByPayloadType.get(selectedPayloadType);
        }
    } catch {
        return undefined;
    }

    return undefined;
};

const findWebRtcVideoCodec = async (endpoint: Endpoint) => {
    const pc = getEndpointPeerConnection(endpoint);
    if (!pc) return undefined;
    return (
        (await findWebRtcVideoCodecFromStats(pc)) ??
        parseVideoCodecFromSdp(pc.remoteDescription?.sdp)
    );
};

const wait = (duration: number) =>
    new Promise<void>((resolve) => {
        window.setTimeout(resolve, duration);
    });

export function Gb28181LivePlayer({ session }: { session: GB28181.PreviewStartResult }) {
    const videoRef = useRef<HTMLVideoElement | null>(null);
    const canvasRef = useRef<HTMLCanvasElement | null>(null);
    const resourcesRef = useRef<PlayerResources>({});
    const [status, setStatus] = useState('准备播放');
    const [activeProtocol, setActiveProtocol] = useState<string>();
    const [surface, setSurface] = useState<'video' | 'canvas'>('video');

    useEffect(() => {
        let disposed = false;
        let fallbackStarted = false;
        const resources = resourcesRef.current;
        const playUrls = session.play_urls;

        const playElement = async (label: string, playingStatus = '播放中') => {
            const video = videoRef.current;
            if (!video || disposed) return;
            setActiveProtocol(label);
            try {
                await video.play();
                if (!disposed) setStatus(playingStatus);
            } catch {
                if (!disposed) setStatus('已就绪');
            }
        };

        const updateWebRtcVideoCodec = async (endpoint: Endpoint) => {
            for (let attempt = 0; attempt < WEBRTC_CODEC_RETRY_COUNT; attempt += 1) {
                if (disposed || resources.rtc !== endpoint) return;
                const codec = await findWebRtcVideoCodec(endpoint);
                if (disposed || resources.rtc !== endpoint) return;
                if (codec) {
                    setStatus(codec);
                    return;
                }
                if (attempt < WEBRTC_CODEC_RETRY_COUNT - 1) {
                    await wait(WEBRTC_CODEC_RETRY_MS);
                }
            }
            if (!disposed && resources.rtc === endpoint) {
                setStatus('未知');
            }
        };

        const playFallbackCandidate = (candidates: PlaybackCandidate[], index: number) => {
            const video = videoRef.current;
            if (!video || disposed) return;

            closePlayerResources(resources, video);
            const candidate = candidates[index];
            if (!candidate) {
                setActiveProtocol(undefined);
                setStatus('没有可用播放地址');
                return;
            }

            const playNext = () => {
                if (!disposed) playFallbackCandidate(candidates, index + 1);
            };

            if (candidate.engine === 'jessibuca') {
                const canvas = canvasRef.current;
                if (!canvas || typeof VideoFrame === 'undefined') {
                    playNext();
                    return;
                }

                setSurface('canvas');
                setActiveProtocol(candidate.label);
                setStatus('连接中');

                let firstFrameRendered = false;
                let player: JessibucaFlvPlayer | undefined;
                let closed = false;
                const firstFrameTimer = window.setTimeout(() => {
                    if (!firstFrameRendered) playNext();
                }, WEBRTC_FALLBACK_DELAY_MS);

                resources.jessibuca = {
                    close: () => {
                        closed = true;
                        window.clearTimeout(firstFrameTimer);
                        player?.close();
                        canvas.getContext('2d')?.clearRect(0, 0, canvas.width, canvas.height);
                    },
                };

                void createJessibucaFlvPlayer({
                    url: candidate.url,
                    canvas,
                    onFirstFrame: () => {
                        if (disposed) return;
                        firstFrameRendered = true;
                        window.clearTimeout(firstFrameTimer);
                        setStatus('播放中');
                    },
                    onError: () => {
                        playNext();
                    },
                })
                    .then((runtime) => {
                        player = runtime;
                        if (closed || disposed) runtime.close();
                    })
                    .catch(() => {
                        playNext();
                    });
                return;
            }

            setSurface('video');
            if (candidate.engine === 'hls') {
                setActiveProtocol(candidate.label);
                setStatus('连接中');
                if (Hls.isSupported()) {
                    const hls = new Hls({
                        backBufferLength: 30,
                        lowLatencyMode: true,
                    });
                    resources.hls = hls;
                    hls.attachMedia(video);
                    hls.on(Hls.Events.MEDIA_ATTACHED, () => {
                        hls.loadSource(candidate.url);
                    });
                    hls.on(Hls.Events.MANIFEST_PARSED, () => {
                        void playElement(candidate.label);
                    });
                    hls.on(Hls.Events.ERROR, (_event, data) => {
                        if (data.fatal) playNext();
                    });
                    return;
                }

                if (video.canPlayType('application/vnd.apple.mpegurl')) {
                    video.src = candidate.url;
                    video.addEventListener(
                        'loadedmetadata',
                        () => void playElement(candidate.label),
                        {
                            once: true,
                        }
                    );
                    video.addEventListener('error', playNext, { once: true });
                    return;
                }

                playNext();
                return;
            }

            if (mpegts.isSupported()) {
                setActiveProtocol(candidate.label);
                setStatus('连接中');
                const player = mpegts.createPlayer(
                    {
                        type: candidate.mediaType ?? 'flv',
                        isLive: true,
                        url: candidate.url,
                    },
                    {
                        enableStashBuffer: false,
                        isLive: true,
                        liveBufferLatencyChasing: true,
                        liveBufferLatencyMaxLatency: 1.5,
                        liveBufferLatencyMinRemain: 0.2,
                    }
                );
                resources.mpegts = player;
                player.on(mpegts.Events.ERROR, playNext);
                player.attachMediaElement(video);
                player.load();
                void playElement(candidate.label);
                return;
            }

            playNext();
        };

        const startFallbackPlayback = () => {
            if (disposed || fallbackStarted) return;
            fallbackStarted = true;
            playFallbackCandidate(buildPlaybackCandidates(playUrls), 0);
        };

        const startWebRtcPlayback = () => {
            const video = videoRef.current;
            if (!video || !playUrls.webrtc || typeof RTCPeerConnection === 'undefined') {
                return false;
            }

            closePlayerResources(resources, video);
            setActiveProtocol('WebRTC');
            setSurface('video');
            setStatus('连接中');

            const endpoint = new Endpoint({
                element: video,
                debug: false,
                zlmsdpUrl: playUrls.webrtc,
                simulcast: false,
                useCamera: false,
                audioEnable: true,
                videoEnable: true,
                recvOnly: true,
                resolution: { w: 0, h: 0 },
                usedatachannel: false,
            });
            resources.rtc = endpoint;

            endpoint.on(Events.WEBRTC_ON_REMOTE_STREAMS, () => {
                if (resources.fallbackTimer) {
                    window.clearTimeout(resources.fallbackTimer);
                    resources.fallbackTimer = undefined;
                }
                void playElement('WebRTC', '获取中').then(() => updateWebRtcVideoCodec(endpoint));
            });
            endpoint.on(Events.WEBRTC_NOT_SUPPORT, startFallbackPlayback);
            endpoint.on(Events.WEBRTC_ICE_CANDIDATE_ERROR, startFallbackPlayback);
            endpoint.on(Events.WEBRTC_OFFER_ANSWER_EXCHANGE_FAILED, startFallbackPlayback);
            endpoint.on(Events.WEBRTC_ON_CONNECTION_STATE_CHANGE, (state: unknown) => {
                if (['closed', 'disconnected', 'failed'].includes(String(state))) {
                    startFallbackPlayback();
                }
            });

            resources.fallbackTimer = window.setTimeout(() => {
                if (video.readyState < video.HAVE_CURRENT_DATA) {
                    startFallbackPlayback();
                }
            }, WEBRTC_FALLBACK_DELAY_MS);
            return true;
        };

        setStatus('准备播放');
        if (!startWebRtcPlayback()) {
            startFallbackPlayback();
        }

        return () => {
            disposed = true;
            closePlayerResources(resources, videoRef.current);
        };
    }, [session.play_urls]);

    return (
        <>
            <video
                ref={videoRef}
                controls
                autoPlay
                muted
                playsInline
                className={`h-full w-full bg-black ${surface === 'video' ? 'block' : 'hidden'}`}
            />
            <canvas
                ref={canvasRef}
                className={`h-full w-full bg-black ${surface === 'canvas' ? 'block' : 'hidden'}`}
            />
            <div className="absolute left-3 top-3 flex items-center gap-2 rounded bg-black/60 px-2 py-1 text-xs text-white">
                {activeProtocol && <span>{activeProtocol}</span>}
                <span>{status}</span>
            </div>
        </>
    );
}
