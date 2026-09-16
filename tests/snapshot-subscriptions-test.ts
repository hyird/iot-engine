import { expect, test } from 'bun:test';
import { SnapshotSubscriptions } from '../web/lib/snapshot-subscriptions';

const settle = (ms = 20) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(predicate: () => boolean) {
    const deadline = Date.now() + 3000;
    while (!predicate() && Date.now() < deadline) await settle(10);
    expect(predicate()).toBe(true);
}
function fixture() {
    let session: { token: string | null; userId: string | null } = { token: 'first', userId: 'user' };
    let active = 0;
    let maximum = 0;
    let opened = 0;
    let pending = 0;
    let maximumRequests = 0;
    let reader: ReadableStreamDefaultController<Uint8Array>;
    const reads: string[] = [];
    const releases: Array<() => void> = [];
    const emit = (event: string, topics: string[]) => reader.enqueue(new TextEncoder().encode(`event: ${event}\ndata: ${JSON.stringify({ topics })}\n\n`));
    const subscriptions = new SnapshotSubscriptions({
        session: () => session,
        refresh: async () => false,
        fetch: (async (input: string, options: RequestInit) => {
            if (input === '/v1/auth/events') {
                opened++;
                active++;
                maximum = Math.max(maximum, active);
                return new Response(new ReadableStream<Uint8Array>({
                    start(controller) { reader = controller; emit('ready', ['*']); },
                    cancel() { active--; },
                }), { headers: { 'content-type': 'text/event-stream' } });
            }
            reads.push(input);
            pending++;
            maximumRequests = Math.max(maximumRequests, pending);
            await settle(15);
            pending--;
            if (input.endsWith('denied')) return new Response('', { status: 403 });
            return Response.json({ code: 0, data: options.headers }, { headers: { 'x-snapshot-topic': input.includes('device') ? 'device' : 'edge' } });
        }) as typeof fetch,
    });
    return {
        subscriptions, reads, emit,
        get maximumRequests() { return maximumRequests; },
        disconnect() { active--; reader.close(); },
        logout() { session = { token: null, userId: null }; subscriptions.sessionChanged(); },
        get active() { return active; }, get maximum() { return maximum; }, get opened() { return opened; },
        switch(token: string) { session = { ...session, token }; subscriptions.sessionChanged(); },
        subscribe(path: string, next = (_value: unknown) => {}, error = (_error: Error) => {}) {
            const release = subscriptions.create(path).subscribe({ next, error }); releases.push(release); return release;
        },
        async close() { for (const release of releases) release(); await settle(); },
    };
}

test('many queries share one SSE; duplicate URLs share snapshots and topics refresh only matching queries', async () => {
    const f = fixture();
    try {
        for (let i = 0; i < 12; i++) f.subscribe(`/v1/device/${i}`);
        f.subscribe('/v1/device/0'); f.subscribe('/v1/edge');
        await until(() => f.reads.length === 13); await settle(40);
        expect(f.opened).toBe(1); expect(f.reads.length).toBe(13);
        expect(f.maximumRequests).toBe(3);
        f.reads.length = 0;
        for (let i = 0; i < 10; i++) f.emit('change', ['edge']);
        await until(() => f.reads.length > 0);
        expect(f.reads).toEqual(['/v1/edge']); expect(f.active).toBe(1);
    } finally { await f.close(); }
    expect(f.active).toBe(0);
});

test('reconnection refreshes all snapshots and logout rejects late responses', async () => {
    const f = fixture(); const values: unknown[] = []; const errors: Error[] = [];
    try {
        f.subscribe('/v1/device', (value) => values.push(value), (error) => errors.push(error));
        await settle(40); f.disconnect(); await settle(1100);
        expect(f.opened).toBe(2); expect(values.length).toBe(2);
        f.emit('change', ['device']); await settle(1); f.logout(); await settle(40);
        expect(values.length).toBe(2); expect(errors.length).toBe(1); expect(f.active).toBe(0);
    } finally { await f.close(); }
});

test('changes during an in-flight snapshot schedule another read', async () => {
    const f = fixture();
    try {
        f.subscribe('/v1/device'); await settle(1);
        f.emit('change', ['device']); await settle(160);
        expect(f.reads).toEqual(['/v1/device', '/v1/device']);
    } finally { await f.close(); }
});

test('token replacement closes old SSE before reconnecting and refreshes active snapshots', async () => {
    const f = fixture(); const values: unknown[] = [];
    try {
        f.subscribe('/v1/device', (value) => values.push(value));
        await settle(); f.switch('second'); await settle(60);
        expect(f.maximum).toBe(1); expect(f.opened).toBe(2);
        expect(values.at(-1)).toEqual({ Accept: 'application/json', Authorization: 'Bearer second' });
    } finally { await f.close(); }
});

test('permission errors isolate the query and cancellation prevents late delivery', async () => {
    const f = fixture(); const errors: Error[] = []; const values: unknown[] = [];
    try {
        f.subscribe('/v1/denied', undefined, (error) => errors.push(error));
        f.subscribe('/v1/device');
        const release = f.subscribe('/v1/edge', (value) => values.push(value));
        await settle(1); release(); await settle(40);
        expect(errors.length).toBe(1); expect(values).toEqual([]); expect(f.active).toBe(1);
    } finally { await f.close(); }
});
