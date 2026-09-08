export interface ServerSentEvent {
    data: string;
    event: string;
    id?: string;
    retry?: number;
}

function parseEventBlock(block: string): ServerSentEvent | null {
    const data: string[] = [];
    let event = 'message';
    let id: string | undefined;
    let retry: number | undefined;

    for (const line of block.split(/\r\n|\r|\n/)) {
        if (!line || line.startsWith(':')) continue;
        const separator = line.indexOf(':');
        const field = separator < 0 ? line : line.slice(0, separator);
        let value = separator < 0 ? '' : line.slice(separator + 1);
        if (value.startsWith(' ')) value = value.slice(1);

        if (field === 'data') data.push(value);
        else if (field === 'event') event = value || 'message';
        else if (field === 'id' && !value.includes('\0')) id = value;
        else if (field === 'retry' && /^\d+$/.test(value)) retry = Number(value);
    }

    if (!data.length) return null;
    return { data: data.join('\n'), event, id, retry };
}

export class ServerSentEventDecoder {
    private buffer = '';

    push(chunk: string): ServerSentEvent[] {
        this.buffer += chunk;
        const events: ServerSentEvent[] = [];
        for (;;) {
            const match = /\r\n\r\n|\n\n|\r\r/.exec(this.buffer);
            if (!match || match.index === undefined) break;
            const block = this.buffer.slice(0, match.index);
            this.buffer = this.buffer.slice(match.index + match[0].length);
            const event = parseEventBlock(block);
            if (event) events.push(event);
        }
        return events;
    }
}

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
