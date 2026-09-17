import { expect, test } from 'bun:test';
import { EdgeDebugConnection } from '../web/pages/iot/edge_node/edge_node.api';
import { DebugSubscriptions } from '../web/pages/iot/edge_node/edge_node.api';
import { SnapshotStream } from '../web/lib/snapshot-stream';

test('explicit refresh waits for a new snapshot, coalesces readers and ignores retired replies', async () => {
    const { channel, sockets } = fixture();
    const subscriptions = new DebugSubscriptions({
        channel, session: () => ({ token: 'token', userId: 'user' }), refresh: async () => false,
    });
    const source = subscriptions.create<number>('edge.terminal.events.subscribe', { page: 1 });
    const values: number[] = [];
    const release = source.subscribe({ next: (value) => values.push(value), error: () => {} });
    await settle();
    const socket = sockets[0]; socket.open(); await settle();
    const oldId = String(socket.sent.find((frame) => frame.event === 'edge.terminal.events.subscribe')!.id);
    socket.reply( oldId, 1);
    expect(await source.first()).toBe(1);
    let resolved = false;
    const fresh = source.map((value) => value * 2).filter((value) => value > 0)
        .first(undefined, { fresh: true }).then((value) => { resolved = true; return value; });
    const concurrent = SnapshotStream.combine([source] as const).first(undefined, { fresh: true });
    await settle();
    expect(resolved).toBe(false);
    expect(sockets).toHaveLength(1);
    const frames = socket.sent.filter((frame) => frame.event === 'edge.terminal.events.subscribe');
    expect(frames).toHaveLength(2);
    expect(socket.sent.filter((frame) => frame.event === 'subscription.cancel')).toHaveLength(1);
    socket.reply( oldId, 99);
    expect(values).toEqual([1]);
    socket.reply( String(frames[1].id), 2);
    expect(await fresh).toBe(4);
    expect(await concurrent).toEqual([2]);
    expect(values).toEqual([1, 2]);
    release(); channel.reset(); await settle();
});

test('query conditions have independent subscriptions and cancelled reads release them', async () => {
    const { channel, sockets } = fixture();
    const subscriptions = new DebugSubscriptions({
        channel, session: () => ({ token: 'token', userId: 'user' }), refresh: async () => false,
    });
    const abort = new AbortController();
    const first = subscriptions.create('edge.terminal.events.subscribe', { page: 1 }).first(abort.signal).catch((error) => error);
    const second = subscriptions.create('edge.terminal.events.subscribe', { page: 2 }).first();
    await settle();
    const socket = sockets[0]; socket.open(); await settle();
    const frames = socket.sent.filter((frame) => frame.event === 'edge.terminal.events.subscribe');
    abort.abort();
    expect((await first).name).toBe('AbortError');
    socket.reply( String(frames[0].id), 'old page');
    socket.reply( String(frames[1].id), 'new page');
    expect(await second).toBe('new page');
    expect(socket.sent.filter((frame) => frame.event === 'subscription.cancel')).toHaveLength(2);
    channel.reset(); await settle();
});

class Socket {
    readyState = 0;
    onopen?: () => void;
    onclose?: () => void;
    onerror?: () => void;
    onmessage?: (event: { data: string }) => void;
    sent: Array<Record<string, unknown>> = [];
    send(value: string) { this.sent.push(JSON.parse(value)); }
    open() { this.readyState = 1; this.onopen?.(); }
    close() { this.readyState = 3; queueMicrotask(() => this.onclose?.()); }
    reply(id: string, data: unknown) {
        this.onmessage?.({ data: JSON.stringify({ id, data }) });
    }
}
const settle = () => new Promise((resolve) => setTimeout(resolve, 0));
function fixture() {
    const sockets: Socket[] = [];
    const channel = new EdgeDebugConnection(() => {
        const socket = new Socket(); sockets.push(socket); return socket as unknown as WebSocket;
    });
    return { channel, sockets };
}
