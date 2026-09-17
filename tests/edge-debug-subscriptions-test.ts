import { expect, test } from 'bun:test';
import { EdgeDebugConnection } from '../web/pages/iot/edge_node/edge_node.api';
import { DebugSubscriptions } from '../web/pages/iot/edge_node/edge_node.api';

class Socket {
    readyState = 0;
    sent: { id: string; event: string; data: unknown }[] = [];
    onopen?: () => void;
    onclose?: () => void;
    onmessage?: (event: { data: string }) => void;
    send(data: string) { this.sent.push(JSON.parse(data)); }
    open() { this.readyState = 1; this.onopen?.(); }
    close() { this.readyState = 3; this.onclose?.(); }
    reply(id: string, data: unknown) { this.onmessage?.({ data: JSON.stringify({ id, data }) }); }
}
const settle = () => new Promise(resolve => setTimeout(resolve, 0));
function fixture() {
    const socket = new Socket();
    const channel = new EdgeDebugConnection(() => socket as unknown as WebSocket);
    const session = { token: 'fixture', userId: 'user-a' };
    const subscriptions = new DebugSubscriptions({ channel, session: () => ({ ...session }), refresh: async () => false });
    return { socket, channel, subscriptions, session };
}

test('equivalent JSON parameters share one subscription and a fresh read replaces the wire ID', async () => {
    const { socket, channel, subscriptions } = fixture();
    const values: unknown[] = [];
    const release = subscriptions.create('edge.terminal.events.subscribe', { page: 1, id: 'device' }).subscribe({ next: v => values.push(v), error: () => {} });
    const first = subscriptions.create('edge.terminal.events.subscribe', { id: 'device', page: 1 }).first();
    await settle(); socket.open(); await settle();
    expect(socket.sent).toHaveLength(1);
    const id = socket.sent[0].id; socket.reply(id, ['first']);
    expect(await first).toEqual(['first']);
    const fresh = subscriptions.create('edge.terminal.events.subscribe', { id: 'device', page: 1 }).first(undefined, { fresh: true });
    await settle();
    expect(socket.sent.map(frame => frame.event)).toEqual(['edge.terminal.events.subscribe', 'subscription.cancel', 'edge.terminal.events.subscribe']);
    socket.reply(id, ['stale']); socket.reply(socket.sent[2].id, ['fresh']);
    expect(await fresh).toEqual(['fresh']); expect(values).toEqual([['first'], ['fresh']]);
    release(); channel.reset();
});

test('one failing consumer cannot stop another consumer of the same snapshot', async () => {
    const { socket, channel, subscriptions } = fixture();
    subscriptions.create('edge.serial.events.subscribe').subscribe({ next: () => { throw new Error('consumer'); }, error: () => { throw new Error('consumer'); } });
    const good = subscriptions.create('edge.serial.events.subscribe').first();
    await settle(); socket.open(); await settle(); socket.reply(socket.sent[0].id, [1]);
    expect(await good).toEqual([1]); expect(socket.readyState).toBe(1);
    channel.reset();
});

test('account replacement drops cached values and terminates old observers', async () => {
    const { socket, channel, subscriptions, session } = fixture();
    const values: unknown[] = []; let failures = 0;
    const release = subscriptions.create('edge.serial.events.subscribe').subscribe({ next: v => values.push(v), error: () => { failures++; } });
    await settle(); socket.open(); await settle(); const id = socket.sent[0].id; socket.reply(id, ['private']);
    session.userId = 'user-b'; session.token = 'other'; subscriptions.sessionChanged();
    socket.reply(id, ['stale']); expect(values).toEqual([['private']]); expect(failures).toBe(1);
    release(); channel.reset();
});
