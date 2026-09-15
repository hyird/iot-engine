import { ServerSentEventDecoder, type ServerSentEvent } from '@/utils/sse';

export async function consumeServerSentEvents(
    stream: ReadableStream<Uint8Array>,
    onEvent: (event: ServerSentEvent) => void,
    signal?: AbortSignal
) {
    const reader = stream.getReader();
    const text = new TextDecoder();
    const decoder = new ServerSentEventDecoder();
    const abort = () => void reader.cancel().catch(() => undefined);
    signal?.addEventListener('abort', abort, { once: true });
    try {
        while (!signal?.aborted) {
            const result = await reader.read();
            if (result.done) break;
            for (const event of decoder.push(text.decode(result.value, { stream: true }))) {
                onEvent(event);
            }
        }
    } finally {
        signal?.removeEventListener('abort', abort);
        await reader.cancel().catch(() => undefined);
        reader.releaseLock();
    }
}
