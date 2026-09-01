import { describe, expect, test } from 'bun:test';
import { reconnectServerSentEvents, ServerSentEventDecoder } from '../web/utils/sse';

describe('server-sent event decoder', () => {
    test('decodes events split across transport chunks', () => {
        const decoder = new ServerSentEventDecoder();
        expect(decoder.push('event: real')).toEqual([]);
        expect(decoder.push('time\r\nid: 42\r\ndata: changed\r\n\r\n')).toEqual([
            { event: 'realtime', id: '42', data: 'changed', retry: undefined },
        ]);
    });

    test('joins multiline data and ignores heartbeat comments', () => {
        const decoder = new ServerSentEventDecoder();
        expect(decoder.push(': keepalive\n\ndata: first\ndata: second\n\n')).toEqual([
            { event: 'message', id: undefined, data: 'first\nsecond', retry: undefined },
        ]);
    });

    test('accepts retry only as a non-negative integer', () => {
        const decoder = new ServerSentEventDecoder();
        expect(decoder.push('retry: 1000\ndata: ready\n\nretry: 1.5\ndata: next\n\n')).toEqual([
            { event: 'message', id: undefined, data: 'ready', retry: 1000 },
            { event: 'message', id: undefined, data: 'next', retry: undefined },
        ]);
    });

    test('reconnects with bounded backoff and resets after a successful connection', async () => {
        const controller = new AbortController();
        const delays: number[] = [];
        let attempts = 0;

        await reconnectServerSentEvents(
            async (connected) => {
                attempts += 1;
                if (attempts === 1) throw new Error('offline');
                if (attempts === 2) {
                    connected();
                    throw new Error('connection dropped');
                }
                controller.abort();
            },
            controller.signal,
            {
                wait: async (delay) => {
                    delays.push(delay);
                },
            }
        );

        expect(attempts).toBe(3);
        expect(delays).toEqual([1_000, 1_000]);
    });
});
