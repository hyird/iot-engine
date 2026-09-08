import { describe, expect, test } from 'bun:test';
import { ServerSentEventDecoder } from '../web/utils/sse';

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
});
