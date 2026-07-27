import { HttpConnection, WebSocketConnection } from 'jv4-connection';
import { VideoDecoderSoftSIMD } from 'jv4-decoder';
import videoDecoderWasmUrl from 'jv4-decoder/wasm/types/videodec_simd.wasm?url';
import { FlvDemuxer } from 'jv4-demuxer/src/flv';
import { CanvasRenderer } from 'jv4-renderer/src/canvas';

const safeCall = (fn) => {
    try {
        fn();
    } catch {}
};

export async function createJessibucaFlvPlayer({ url, canvas, onFirstFrame, onError }) {
    let closed = false;
    const abortController = new AbortController();
    const connection = url.startsWith('ws')
        ? new WebSocketConnection(url)
        : new HttpConnection(url);
    const videoDecoder = new VideoDecoderSoftSIMD({
        wasmPath: videoDecoderWasmUrl,
        workerMode: false,
    });
    const renderer = new CanvasRenderer(canvas);

    const close = () => {
        if (closed) return;
        closed = true;
        abortController.abort();
        safeCall(() => connection.close());
        safeCall(() => videoDecoder.close());
        safeCall(() => renderer.close());
    };

    videoDecoder.on('videoFrame', (videoFrame) => {
        if (closed) {
            videoFrame.close();
            return;
        }
        renderer.writeVideo(videoFrame);
        onFirstFrame?.();
    });
    videoDecoder.on('error', (error) => {
        onError?.(error);
        close();
    });

    await videoDecoder.initialize();
    if (closed) return { close };

    await connection.connect();
    if (closed) return { close };

    const demuxer = new FlvDemuxer(connection);
    demuxer.on('video-encoder-config-changed', (config) => {
        videoDecoder.configure(config);
    });
    demuxer.on('demux-error', (error) => {
        onError?.(error);
        close();
    });
    const pipeErrorHandler = (error) => {
        if (!closed && !abortController.signal.aborted) {
            onError?.(error);
            close();
        }
    };

    demuxer.audioReadable
        ?.pipeTo(
            new WritableStream({
                write() {},
            }),
            { signal: abortController.signal }
        )
        .catch(pipeErrorHandler);

    demuxer.videoReadable
        ?.pipeTo(
            new WritableStream({
                write(chunk) {
                    if (!closed) videoDecoder.decode(chunk);
                },
            }),
            { signal: abortController.signal }
        )
        .catch(pipeErrorHandler);

    return { close };
}
