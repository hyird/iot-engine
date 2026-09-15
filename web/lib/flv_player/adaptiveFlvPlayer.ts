import videoDecoderWasmUrl from 'jv4-decoder/wasm/types/videodec_simd.wasm?url';
import {
    AudioDecoderSoft,
    DemuxMode,
    FlvDemuxer,
    HttpConnection,
    VideoDecoderSoftSIMD,
    WebSocketConnection,
} from './jv4Runtime';

const MAX_NATIVE_DECODE_QUEUE = 3;
const MAX_SOFTWARE_DECODE_BACKLOG = 3;
const MAX_AUDIO_LEAD_SECONDS = 0.25;

type DecoderPreference = 'native-only' | 'software-only';
type VideoDecoderKind = 'wasm-worker' | 'webcodecs';

type VideoDecoderRuntime = {
    close: () => void;
    decode: (chunk: EncodedVideoChunkInit) => void;
    kind: VideoDecoderKind;
};

type AudioDecoderRuntime = {
    close: () => void;
    decode: (chunk: EncodedAudioChunkInit) => void;
};

export type AdaptiveFlvPlayer = {
    close: () => void;
    setMuted: (muted: boolean) => void;
};

export type AdaptiveFlvPlaybackInfo = {
    codec: string;
    decoder: VideoDecoderKind;
};

type AdaptiveFlvPlayerOptions = {
    canvas: HTMLCanvasElement;
    decoder: DecoderPreference;
    muted: boolean;
    onAudioAvailable?: (available: boolean) => void;
    onError?: (error: unknown) => void;
    onFirstFrame?: (info: AdaptiveFlvPlaybackInfo) => void;
    onVideoMode?: (info: AdaptiveFlvPlaybackInfo) => void;
    url: string;
};

const cloneBufferSource = (
    source?: AllowSharedBufferSource
): Uint8Array<ArrayBuffer> | undefined => {
    if (!source) return undefined;
    const bytes = ArrayBuffer.isView(source)
        ? new Uint8Array(source.buffer, source.byteOffset, source.byteLength)
        : new Uint8Array(source);
    return Uint8Array.from(bytes);
};

const hexByte = (value: number) => value.toString(16).padStart(2, '0').toUpperCase();

const reverse32BitInt = (value: number) => {
    let result = 0;
    for (let bit = 0; bit < 32; bit += 1) {
        result |= ((value >>> bit) & 1) << (31 - bit);
    }
    return result >>> 0;
};

const avcCodec = (description?: AllowSharedBufferSource) => {
    const bytes = cloneBufferSource(description);
    if (!bytes || bytes.byteLength < 4) return 'avc1.420028';
    return `avc1.${hexByte(bytes[1])}${hexByte(bytes[2])}${hexByte(bytes[3])}`;
};

const hevcCodec = (description?: AllowSharedBufferSource) => {
    const bytes = cloneBufferSource(description);
    if (!bytes || bytes.byteLength < 13) return 'hvc1.1.6.L93.B0';

    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const profileByte = bytes[1];
    const profileSpace = ['', 'A', 'B', 'C'][profileByte >>> 6];
    const profile = profileByte & 0x1f;
    const compatibility = reverse32BitInt(view.getUint32(2)).toString(16).toUpperCase();
    const tier = profileByte & 0x20 ? 'H' : 'L';
    const level = bytes[12];
    let constraints = '';
    for (let index = 11; index >= 6; index -= 1) {
        const value = bytes[index];
        if (value || constraints)
            constraints = `.${value.toString(16).toUpperCase()}${constraints}`;
    }
    return `hvc1.${profileSpace}${profile}.${compatibility}.${tier}${level}${constraints}`;
};

const nativeVideoConfig = (config: VideoDecoderConfig): VideoDecoderConfig => {
    const description = cloneBufferSource(config.description);
    const codec = config.codec.toLowerCase();
    return {
        ...config,
        codec:
            codec === 'hevc'
                ? hevcCodec(description)
                : codec === 'avc'
                  ? avcCodec(description)
                  : codec === 'av1'
                    ? 'av01.0.05M.08'
                    : config.codec,
        description,
        hardwareAcceleration: 'prefer-hardware',
        optimizeForLatency: true,
    };
};

const nativeAudioConfig = (config: AudioDecoderConfig): AudioDecoderConfig => ({
    ...config,
    codec:
        config.codec === 'pcma'
            ? 'alaw'
            : config.codec === 'pcmu'
              ? 'ulaw'
              : config.codec === 'aac'
                ? 'mp4a.40.2'
                : config.codec,
    description: cloneBufferSource(config.description),
});

const codecLabel = (codec: string) => {
    const normalized = codec.toLowerCase();
    if (normalized === 'hevc' || normalized.startsWith('hvc1') || normalized.startsWith('hev1')) {
        return 'H.265';
    }
    if (normalized === 'avc' || normalized.startsWith('avc1')) return 'H.264';
    if (normalized === 'av1' || normalized.startsWith('av01')) return 'AV1';
    return codec.toUpperCase();
};

class LowLatencyRenderer {
    private audioContext?: AudioContext;
    private muted: boolean;
    private nextAudioStart = 0;
    private readonly context: CanvasRenderingContext2D;

    constructor(
        private readonly canvas: HTMLCanvasElement,
        muted: boolean
    ) {
        const context = canvas.getContext('2d');
        if (!context) throw new Error('浏览器不支持 Canvas 2D 渲染');
        this.context = context;
        this.muted = muted;
    }

    setMuted(muted: boolean) {
        this.muted = muted;
        if (!muted) void this.ensureAudioContext()?.resume();
    }

    writeVideo(frame: VideoFrame) {
        if (
            this.canvas.width !== frame.displayWidth ||
            this.canvas.height !== frame.displayHeight
        ) {
            this.canvas.width = frame.displayWidth;
            this.canvas.height = frame.displayHeight;
        }
        this.context.drawImage(frame, 0, 0, frame.displayWidth, frame.displayHeight);
        frame.close();
    }

    writeAudio(data: AudioData) {
        if (this.muted) {
            data.close();
            return;
        }
        const audioContext = this.ensureAudioContext();
        if (!audioContext) {
            data.close();
            return;
        }

        const buffer = audioContext.createBuffer(
            data.numberOfChannels,
            data.numberOfFrames,
            data.sampleRate
        );
        for (let channel = 0; channel < data.numberOfChannels; channel += 1) {
            const values = new Float32Array(data.numberOfFrames);
            data.copyTo(values, { planeIndex: channel });
            buffer.copyToChannel(values, channel);
        }
        data.close();

        const now = audioContext.currentTime;
        if (this.nextAudioStart < now || this.nextAudioStart - now > MAX_AUDIO_LEAD_SECONDS) {
            this.nextAudioStart = now + 0.02;
        }
        const source = audioContext.createBufferSource();
        source.buffer = buffer;
        source.connect(audioContext.destination);
        source.start(this.nextAudioStart);
        this.nextAudioStart += buffer.duration;
    }

    close() {
        this.context.clearRect(0, 0, this.canvas.width, this.canvas.height);
        if (this.audioContext) void this.audioContext.close();
        this.audioContext = undefined;
    }

    private ensureAudioContext() {
        if (typeof AudioContext === 'undefined') return undefined;
        this.audioContext ??= new AudioContext({ latencyHint: 'interactive' });
        return this.audioContext;
    }
}

export async function createAdaptiveFlvPlayer({
    canvas,
    decoder: decoderPreference,
    muted,
    onAudioAvailable,
    onError,
    onFirstFrame,
    onVideoMode,
    url,
}: AdaptiveFlvPlayerOptions): Promise<AdaptiveFlvPlayer> {
    let closed = false;
    let audioConfig: AudioDecoderConfig | undefined;
    let audioDecoderPromise: Promise<AudioDecoderRuntime | undefined> | undefined;
    let audioMuted = muted;
    let firstFrameRendered = false;
    let videoDecoderPromise: Promise<VideoDecoderRuntime> | undefined;
    let videoGeneration = 0;
    const abortController = new AbortController();
    const renderer = new LowLatencyRenderer(canvas, muted);
    const connection = url.startsWith('ws')
        ? new WebSocketConnection(url)
        : new HttpConnection(url);

    const fail = (error: unknown) => {
        if (!closed) onError?.(error);
    };

    const createSoftwareVideoDecoder = async (
        config: VideoDecoderConfig
    ): Promise<VideoDecoderRuntime> => {
        const decoder = new VideoDecoderSoftSIMD({
            wasmPath: videoDecoderWasmUrl,
            workerMode: true,
        });
        let backlog = 0;
        let waitingForKeyFrame = false;
        decoder.on('videoFrame', (frame: VideoFrame) => {
            backlog = Math.max(0, backlog - 1);
            if (closed) {
                frame.close();
                return;
            }
            renderer.writeVideo(frame);
            if (!firstFrameRendered) {
                firstFrameRendered = true;
                onFirstFrame?.({ codec: codecLabel(config.codec), decoder: 'wasm-worker' });
            }
        });
        decoder.on('error', fail);
        await decoder.initialize();
        if (closed) {
            decoder.worker?.terminate();
            decoder.close();
            throw new Error('播放器已关闭');
        }
        decoder.worker?.addEventListener('error', fail);
        decoder.configure(config);
        onVideoMode?.({ codec: codecLabel(config.codec), decoder: 'wasm-worker' });
        return {
            kind: 'wasm-worker',
            decode: (chunk) => {
                if (backlog >= MAX_SOFTWARE_DECODE_BACKLOG) waitingForKeyFrame = true;
                if (waitingForKeyFrame) {
                    if (chunk.type !== 'key') return;
                    waitingForKeyFrame = false;
                    backlog = 0;
                }
                backlog += 1;
                decoder.decode(chunk);
            },
            close: () => {
                decoder.worker?.terminate();
                decoder.close();
            },
        };
    };

    const createNativeVideoDecoder = async (
        config: VideoDecoderConfig
    ): Promise<VideoDecoderRuntime> => {
        if (typeof VideoDecoder === 'undefined' || typeof EncodedVideoChunk === 'undefined') {
            throw new Error('浏览器不支持 WebCodecs');
        }
        const normalized = nativeVideoConfig(config);
        const support = await VideoDecoder.isConfigSupported(normalized);
        if (!support.supported) throw new Error(`浏览器不支持 ${codecLabel(config.codec)} 硬解`);

        const decoder = new VideoDecoder({
            output: (frame) => {
                if (closed) {
                    frame.close();
                    return;
                }
                renderer.writeVideo(frame);
                if (!firstFrameRendered) {
                    firstFrameRendered = true;
                    onFirstFrame?.({ codec: codecLabel(config.codec), decoder: 'webcodecs' });
                }
            },
            error: fail,
        });
        decoder.configure(support.config ?? normalized);
        onVideoMode?.({ codec: codecLabel(config.codec), decoder: 'webcodecs' });
        let waitingForKeyFrame = false;
        return {
            kind: 'webcodecs',
            decode: (chunk) => {
                if (decoder.decodeQueueSize >= MAX_NATIVE_DECODE_QUEUE) waitingForKeyFrame = true;
                if (waitingForKeyFrame) {
                    if (chunk.type !== 'key') return;
                    decoder.reset();
                    decoder.configure(support.config ?? normalized);
                    waitingForKeyFrame = false;
                }
                decoder.decode(new EncodedVideoChunk(chunk));
            },
            close: () => {
                if (decoder.state !== 'closed') decoder.close();
            },
        };
    };

    const configureVideo = (config: VideoDecoderConfig) => {
        const generation = ++videoGeneration;
        const previous = videoDecoderPromise;
        videoDecoderPromise = (async () => {
            const previousDecoder = await previous?.catch(() => undefined);
            previousDecoder?.close();
            const decoder =
                decoderPreference === 'native-only'
                    ? await createNativeVideoDecoder(config)
                    : await createSoftwareVideoDecoder(config);
            if (closed || generation !== videoGeneration) {
                decoder.close();
                throw new Error('视频编码配置已更新');
            }
            return decoder;
        })();
        videoDecoderPromise.catch(fail);
    };

    const createAudioDecoder = async (config: AudioDecoderConfig) => {
        const writeAudio = (data: AudioData) => {
            if (closed) data.close();
            else renderer.writeAudio(data);
        };
        if (typeof AudioDecoder !== 'undefined' && typeof EncodedAudioChunk !== 'undefined') {
            const normalized = nativeAudioConfig(config);
            const support = await AudioDecoder.isConfigSupported(normalized).catch(() => undefined);
            if (support?.supported) {
                const decoder = new AudioDecoder({ output: writeAudio, error: fail });
                decoder.configure(support.config ?? normalized);
                return {
                    decode: (chunk: EncodedAudioChunkInit) =>
                        decoder.decode(new EncodedAudioChunk(chunk)),
                    close: () => {
                        if (decoder.state !== 'closed') decoder.close();
                    },
                } satisfies AudioDecoderRuntime;
            }
        }

        const decoder = new AudioDecoderSoft();
        decoder.on('audioFrame', writeAudio);
        decoder.on('error', fail);
        await decoder.initialize();
        decoder.configure(config);
        return {
            decode: (chunk) => decoder.decode(chunk),
            close: () => decoder.close(),
        } satisfies AudioDecoderRuntime;
    };

    const ensureAudioDecoder = () => {
        if (!audioConfig) return Promise.resolve(undefined);
        audioDecoderPromise ??= createAudioDecoder(audioConfig);
        audioDecoderPromise.catch(fail);
        return audioDecoderPromise;
    };

    await connection.connect();
    if (closed) throw new Error('播放器已关闭');

    const demuxer = new FlvDemuxer(connection, DemuxMode.PULL, 'avcc');
    demuxer.on('video-encoder-config-changed', configureVideo);
    demuxer.on('audio-encoder-config-changed', (config: AudioDecoderConfig) => {
        const previous = audioDecoderPromise;
        audioConfig = config;
        audioDecoderPromise = undefined;
        void previous?.then((decoder) => decoder?.close()).catch(() => undefined);
        onAudioAvailable?.(true);
    });
    demuxer.on('demux-error', fail);

    const pipeError = (error: unknown) => {
        if (!closed && !abortController.signal.aborted) fail(error);
    };
    void demuxer.videoReadable
        ?.pipeTo(
            new WritableStream({
                async write(chunk: EncodedVideoChunkInit) {
                    const decoder = await videoDecoderPromise;
                    if (!closed && decoder) decoder.decode(chunk);
                },
            }),
            { signal: abortController.signal }
        )
        .catch(pipeError);
    void demuxer.audioReadable
        ?.pipeTo(
            new WritableStream({
                async write(chunk: EncodedAudioChunkInit) {
                    if (closed || audioMuted || !audioConfig) return;
                    const decoder = await ensureAudioDecoder();
                    if (!closed) decoder?.decode(chunk);
                },
            }),
            { signal: abortController.signal }
        )
        .catch(pipeError);

    return {
        setMuted: (nextMuted) => {
            audioMuted = nextMuted;
            renderer.setMuted(nextMuted);
        },
        close: () => {
            if (closed) return;
            closed = true;
            abortController.abort();
            try {
                connection.close();
            } catch {}
            void videoDecoderPromise?.then((decoder) => decoder.close()).catch(() => undefined);
            void audioDecoderPromise?.then((decoder) => decoder?.close()).catch(() => undefined);
            renderer.close();
        },
    };
}
