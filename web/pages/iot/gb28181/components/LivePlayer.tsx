import Hls from 'hls.js';
import mpegts from 'mpegts.js';
import { useEffect, useRef, useState } from 'react';
import { Endpoint, Events } from 'zlmrtc-client';
import { type AdaptiveFlvPlayer, createAdaptiveFlvPlayer } from '@/lib/gb28181/adaptiveFlvPlayer';
import type { GB28181 } from '../gb28181.types';
import {
    buildPlaybackCandidates,
    type PlaybackCandidate,
    type PlaybackCapabilities,
} from '../playbackCandidates';

const WEBRTC_CONNECT_TIMEOUT_MS = 4000;
const FALLBACK_FIRST_FRAME_TIMEOUT_MS = 5000;
const WEBRTC_CODEC_RETRY_MS = 400;
const WEBRTC_CODEC_RETRY_COUNT = 10;

type PlayerResources = {
    adaptiveFlv?: AdaptiveFlvPlayer;
    candidateCleanup?: () => void;
    hls?: Hls;
    mpegts?: ReturnType<typeof mpegts.createPlayer>;
    rtc?: Endpoint;
    fallbackTimer?: number;
};

const resetVideoElement = (video: HTMLVideoElement) => {
    video.pause();
    video.removeAttribute('src');
    video.srcObject = null;
    video.load();
};

const closePlayerResources = (resources: PlayerResources, video?: HTMLVideoElement | null) => {
    resources.candidateCleanup?.();
    resources.candidateCleanup = undefined;
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
    resources.adaptiveFlv?.close();
    resources.adaptiveFlv = undefined;
    if (video) resetVideoElement(video);
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

const watchFirstVideoFrame = (video: HTMLVideoElement, callback: () => void) => {
    let done = false;
    const finish = () => {
        if (done) return;
        done = true;
        callback();
    };

    if (typeof video.requestVideoFrameCallback === 'function') {
        const id = video.requestVideoFrameCallback(finish);
        return () => {
            done = true;
            video.cancelVideoFrameCallback(id);
        };
    }

    video.addEventListener('loadeddata', finish, { once: true });
    return () => {
        done = true;
        video.removeEventListener('loadeddata', finish);
    };
};

const detectPlaybackCapabilities = (video: HTMLVideoElement): PlaybackCapabilities => {
    const featureList = mpegts.getFeatureList();
    return {
        hls: Hls.isSupported() || Boolean(video.canPlayType('application/vnd.apple.mpegurl')),
        mpegts: mpegts.isSupported(),
        mseH265: featureList.mseH265Playback === true,
        softwareVideo:
            typeof Worker !== 'undefined' &&
            typeof WebAssembly !== 'undefined' &&
            typeof VideoFrame !== 'undefined',
        webCodecs:
            typeof VideoDecoder !== 'undefined' &&
            typeof EncodedVideoChunk !== 'undefined' &&
            typeof VideoFrame !== 'undefined',
    };
};

export function Gb28181LivePlayer({ session }: { session: GB28181.PreviewStartResult }) {
    const videoRef = useRef<HTMLVideoElement | null>(null);
    const canvasRef = useRef<HTMLCanvasElement | null>(null);
    const resourcesRef = useRef<PlayerResources>({});
    const mutedRef = useRef(true);
    const [status, setStatus] = useState('准备播放');
    const [activeProtocol, setActiveProtocol] = useState<string>();
    const [audioAvailable, setAudioAvailable] = useState(false);
    const [muted, setMuted] = useState(true);
    const [surface, setSurface] = useState<'video' | 'canvas'>('video');

    useEffect(() => {
        let disposed = false;
        let fallbackStarted = false;
        const resources = resourcesRef.current;
        const playUrls = session.play_urls;

        setAudioAvailable(false);

        const playElement = async (label: string) => {
            const video = videoRef.current;
            if (!video || disposed) return;
            setActiveProtocol(label);
            try {
                await video.play();
            } catch {
                if (!disposed) setStatus('等待浏览器允许播放');
            }
        };

        const updateWebRtcVideoCodec = async (
            endpoint: Endpoint,
            onCodec: (codec: string) => void
        ) => {
            for (let attempt = 0; attempt < WEBRTC_CODEC_RETRY_COUNT; attempt += 1) {
                if (disposed || resources.rtc !== endpoint) return;
                const codec = await findWebRtcVideoCodec(endpoint);
                if (disposed || resources.rtc !== endpoint) return;
                if (codec) {
                    onCodec(codec);
                    return;
                }
                if (attempt < WEBRTC_CODEC_RETRY_COUNT - 1) {
                    await wait(WEBRTC_CODEC_RETRY_MS);
                }
            }
            if (!disposed && resources.rtc === endpoint) onCodec('未知编码');
        };

        const playFallbackCandidate = (candidates: PlaybackCandidate[], index: number) => {
            const video = videoRef.current;
            const canvas = canvasRef.current;
            if (!video || !canvas || disposed) return;

            closePlayerResources(resources, video);
            const candidate = candidates[index];
            if (!candidate) {
                setActiveProtocol(undefined);
                setStatus('没有可用播放地址');
                return;
            }

            let candidateClosed = false;
            let advanced = false;
            const startedAt = performance.now();
            const firstFrameTimer = window.setTimeout(() => {
                playNext();
            }, FALLBACK_FIRST_FRAME_TIMEOUT_MS);
            let stopWatchingFrame: (() => void) | undefined;
            const cleanupCandidate = () => {
                candidateClosed = true;
                advanced = true;
                window.clearTimeout(firstFrameTimer);
                stopWatchingFrame?.();
            };
            const playNext = () => {
                if (advanced || disposed) return;
                advanced = true;
                window.clearTimeout(firstFrameTimer);
                stopWatchingFrame?.();
                playFallbackCandidate(candidates, index + 1);
            };
            const firstFrameStatus = (details?: string) => {
                window.clearTimeout(firstFrameTimer);
                const elapsed = Math.max(0, Math.round(performance.now() - startedAt));
                setStatus(`${details ? `${details} · ` : ''}首帧 ${elapsed}ms`);
            };
            resources.candidateCleanup = cleanupCandidate;
            setAudioAvailable(false);
            setActiveProtocol(candidate.label);
            setStatus('连接中');

            if (candidate.engine === 'adaptive-flv') {
                setSurface('canvas');
                void createAdaptiveFlvPlayer({
                    url: candidate.url,
                    canvas,
                    decoder: candidate.decoder ?? 'native-only',
                    muted: mutedRef.current,
                    onAudioAvailable: (available) => {
                        if (!disposed && !candidateClosed) setAudioAvailable(available);
                    },
                    onVideoMode: (info) => {
                        if (!disposed && !candidateClosed) {
                            setStatus(
                                `${info.codec} · ${info.decoder === 'webcodecs' ? '硬解' : 'Worker软解'} · 解码中`
                            );
                        }
                    },
                    onFirstFrame: (info) => {
                        if (disposed || candidateClosed) return;
                        firstFrameStatus(
                            `${info.codec} · ${info.decoder === 'webcodecs' ? '硬解' : 'Worker软解'}`
                        );
                    },
                    onError: playNext,
                })
                    .then((runtime) => {
                        if (candidateClosed || disposed || advanced) runtime.close();
                        else resources.adaptiveFlv = runtime;
                    })
                    .catch(playNext);
                return;
            }

            setSurface('video');
            stopWatchingFrame = watchFirstVideoFrame(video, () => firstFrameStatus());
            if (candidate.engine === 'hls') {
                if (Hls.isSupported()) {
                    const hls = new Hls({
                        backBufferLength: 5,
                        liveMaxLatencyDurationCount: 3,
                        liveSyncDurationCount: 1,
                        lowLatencyMode: true,
                    });
                    resources.hls = hls;
                    hls.attachMedia(video);
                    hls.on(Hls.Events.MEDIA_ATTACHED, () => {
                        hls.loadSource(candidate.url);
                    });
                    hls.on(Hls.Events.MANIFEST_PARSED, () => {
                        setAudioAvailable(hls.audioTracks.length > 0);
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
                        () => {
                            void playElement(candidate.label);
                        },
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
                        liveBufferLatencyMaxLatency: 1,
                        liveBufferLatencyMinRemain: 0.1,
                        lazyLoad: false,
                    }
                );
                resources.mpegts = player;
                player.on(mpegts.Events.ERROR, playNext);
                player.on(mpegts.Events.MEDIA_INFO, (info) => {
                    if (candidateClosed || disposed) return;
                    setAudioAvailable(info.hasAudio === true);
                    if (info.videoCodec)
                        setStatus(`${info.videoCodec.toUpperCase()} · 原生解码 · 解码中`);
                });
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
            const video = videoRef.current;
            if (!video) return;
            playFallbackCandidate(
                buildPlaybackCandidates(playUrls, detectPlaybackCapabilities(video)),
                0
            );
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
            const startedAt = performance.now();
            let codec = '未知编码';
            let firstFrameElapsed: number | undefined;
            const renderWebRtcStatus = () => {
                setStatus(
                    firstFrameElapsed === undefined
                        ? `${codec} · 获取中`
                        : `${codec} · 首帧 ${firstFrameElapsed}ms`
                );
            };
            const stopWatchingFrame = watchFirstVideoFrame(video, () => {
                firstFrameElapsed = Math.max(0, Math.round(performance.now() - startedAt));
                if (resources.fallbackTimer) {
                    window.clearTimeout(resources.fallbackTimer);
                    resources.fallbackTimer = undefined;
                }
                renderWebRtcStatus();
            });
            resources.candidateCleanup = stopWatchingFrame;

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
                const stream = video.srcObject;
                setAudioAvailable(
                    stream instanceof MediaStream && stream.getAudioTracks().length > 0
                );
                void playElement('WebRTC').then(() =>
                    updateWebRtcVideoCodec(endpoint, (nextCodec) => {
                        codec = nextCodec;
                        renderWebRtcStatus();
                    })
                );
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
                resources.fallbackTimer = undefined;
                if (video.readyState < video.HAVE_CURRENT_DATA) {
                    startFallbackPlayback();
                }
            }, WEBRTC_CONNECT_TIMEOUT_MS);
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

    const toggleMuted = () => {
        const nextMuted = !mutedRef.current;
        mutedRef.current = nextMuted;
        setMuted(nextMuted);
        if (videoRef.current) videoRef.current.muted = nextMuted;
        resourcesRef.current.adaptiveFlv?.setMuted(nextMuted);
    };

    return (
        <>
            <video
                ref={videoRef}
                controls
                autoPlay
                muted={muted}
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
            {audioAvailable && (
                <button
                    type="button"
                    onClick={toggleMuted}
                    className="absolute bottom-3 left-3 z-20 rounded bg-black/60 px-2 py-1 text-xs text-white transition hover:bg-black/80"
                >
                    {muted ? '开启声音' : '关闭声音'}
                </button>
            )}
        </>
    );
}
