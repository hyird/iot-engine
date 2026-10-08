export class HttpConnection {
    constructor(url: string);
    close(): void;
    connect(): Promise<void>;
}

export class WebSocketConnection extends HttpConnection {}

export const DemuxMode: {
    PULL: number;
    PUSH: number;
};

export class FlvDemuxer {
    constructor(source: HttpConnection, mode: number, format: 'annexb' | 'avcc');
    audioReadable?: ReadableStream<EncodedAudioChunkInit>;
    videoReadable?: ReadableStream<EncodedVideoChunkInit>;
    on<TArgs extends unknown[]>(event: string, handler: (...args: TArgs) => void): void;
}

export class VideoDecoderSoftSIMD {
    constructor(options: { wasmPath: string; workerMode: boolean });
    worker?: Worker;
    close(): void;
    configure(config: VideoDecoderConfig): void;
    decode(chunk: EncodedVideoChunkInit): void;
    initialize(): Promise<void>;
    on<TArgs extends unknown[]>(event: string, handler: (...args: TArgs) => void): void;
}

export class AudioDecoderSoft {
    close(): void;
    configure(config: AudioDecoderConfig): void;
    decode(chunk: EncodedAudioChunkInit): void;
    initialize(): Promise<void>;
    on<TArgs extends unknown[]>(event: string, handler: (...args: TArgs) => void): void;
}
