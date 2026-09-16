import { afterEach, expect, mock, test } from 'bun:test';
import { trackHttpRequest } from '../web/lib/http-activity';

mock.module('../web/store/authStore', () => ({
    useAuthStore: { getState: () => ({ token: 'test-token' }) },
}));
mock.module('../web/lib/http', () => ({ refreshSession: async () => false }));
const { createSnapshotStream } = await import('../web/lib/snapshot-request');

const originalFetch = globalThis.fetch;
const releases: Array<() => void> = [];
afterEach(() => {
    for (const release of releases.splice(0)) release();
    globalThis.fetch = originalFetch;
});

async function settle() {
    await new Promise((resolve) => setTimeout(resolve, 0));
}

function serveSnapshots() {
    const active = new Set<AbortSignal>();
    let opened = 0;
    globalThis.fetch = mock(async (_url: unknown, init?: RequestInit) => {
        const signal = init?.signal as AbortSignal;
        if (signal.aborted) throw new DOMException('Aborted', 'AbortError');
        active.add(signal);
        opened++;
        return new Response(
            new ReadableStream<Uint8Array>({
                start(controller) {
                    controller.enqueue(
                        new TextEncoder().encode(
                            `event: snapshot\ndata: ${JSON.stringify({ code: 0, data: opened })}\n\n`
                        )
                    );
                },
                cancel() {
                    active.delete(signal);
                },
            }),
            { headers: { 'content-type': 'text/event-stream' } }
        );
    }) as unknown as typeof fetch;
    return { active, opened: () => opened };
}

test('six SSE sockets yield to overlapping writes and resume with fresh snapshots', async () => {
    const server = serveSnapshots();
    const values: number[] = [];
    const errors: Error[] = [];
    for (let index = 0; index < 6; index++) {
        releases.push(
            createSnapshotStream<number>(`/snapshot/${index}`).subscribe({
                next: (value) => values.push(value),
                error: (error) => errors.push(error),
            })
        );
    }
    await settle();
    expect(server.active.size).toBe(6);
    let finishFirst = () => {};
    let finishSecond = () => {};
    const first = trackHttpRequest(() => new Promise<void>((resolve) => (finishFirst = resolve)));
    const second = trackHttpRequest(() => new Promise<void>((resolve) => (finishSecond = resolve)));
    await settle();
    expect(server.active.size).toBe(0);
    expect(values).toHaveLength(6);
    finishFirst();
    await first;
    await settle();
    expect(server.active.size).toBe(0);
    finishSecond();
    await second;
    await settle();
    expect(server.active.size).toBe(6);
    expect(values).toHaveLength(12);
    expect(errors).toEqual([]);
});

test('failed writes restore streams, while subscribers removed during a write stay closed', async () => {
    const server = serveSnapshots();
    const errors: Error[] = [];
    const release = createSnapshotStream('/cancelled').subscribe({
        next: () => {},
        error: (error) => errors.push(error),
    });
    releases.push(release);
    releases.push(
        createSnapshotStream('/retained').subscribe({
            next: () => {},
            error: (error) => errors.push(error),
        })
    );
    await settle();
    await expect(
        trackHttpRequest(async () => {
            release();
            throw new Error('write failed');
        })
    ).rejects.toThrow('write failed');
    await settle();
    expect(server.active.size).toBe(1);
    expect(server.opened()).toBe(3);
    expect(errors).toEqual([]);
    const value = createSnapshotStream('/after-failure').first();
    expect(await value).toBe(4);
});

test('subscriptions started during a write wait, and can be cancelled before opening a socket', async () => {
    const server = serveSnapshots();
    let finish = () => {};
    const write = trackHttpRequest(() => new Promise<void>((resolve) => (finish = resolve)));
    const abort = new AbortController();
    const snapshot = createSnapshotStream('/never-opened').first(abort.signal);
    void snapshot.catch(() => {});
    await settle();
    expect(server.opened()).toBe(0);
    abort.abort(new Error('cancelled'));
    await expect(snapshot).rejects.toThrow('cancelled');
    finish();
    await write;
    await settle();
    expect(server.opened()).toBe(0);
});
