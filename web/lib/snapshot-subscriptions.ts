import { type SnapshotObserver, SnapshotStream } from './snapshot-stream';
import { consumeServerSentEvents } from './sse';

interface Session {
    token: string | null;
    userId: string | null;
}
interface Subscription {
    url: string;
    observers: Set<SnapshotObserver<unknown>>;
    topic?: string;
    value?: unknown;
    received: boolean;
    dirty: boolean;
    pending?: AbortController;
    timer?: ReturnType<typeof setTimeout>;
    nextRequest: number;
    coalesce: number;
}
class SnapshotError extends Error {
    constructor(
        message: string,
        readonly terminal = false,
        readonly expired = false
    ) {
        super(message);
    }
}

/** One authenticated event connection per application, with bounded snapshot requests. */
export class SnapshotSubscriptions {
    private readonly subscriptions = new Map<string, Subscription>();
    private stream?: AbortController;
    private ready = false;
    private running = 0;
    private generation = 0;
    private retry?: ReturnType<typeof setTimeout>;
    private backoff = 1000;
    private refreshing?: Promise<boolean>;
    private session: Session;
    private readonly fetch: typeof fetch;

    constructor(
        private readonly options: {
            session: () => Session;
            refresh: () => Promise<boolean>;
            fetch?: typeof fetch;
        }
    ) {
        this.session = options.session();
        this.fetch = options.fetch ?? globalThis.fetch.bind(globalThis);
    }

    sessionChanged() {
        const next = this.options.session();
        if (next.token === this.session.token && next.userId === this.session.userId) return;
        const changedAccount = next.userId !== this.session.userId || !next.token;
        this.session = next;
        this.stop();
        if (changedAccount) this.failAll(new SnapshotError('登录状态已变更', true));
        for (const entry of this.subscriptions.values()) {
            entry.dirty = true;
            entry.nextRequest = 0;
        }
        this.ensureStream();
    }

    create<T>(url: string): SnapshotStream<T> {
        return new SnapshotStream((observer) => {
            this.sessionChanged();
            if (!this.session.token || !url.startsWith('/v1/') || /[\\\r\n#]/.test(url)) {
                observer.error(new SnapshotError('登录状态或查询地址无效', true));
                return () => {};
            }
            let entry = this.subscriptions.get(url);
            if (!entry) {
                entry = {
                    url,
                    observers: new Set(),
                    received: false,
                    dirty: true,
                    nextRequest: 0,
                    coalesce: 100,
                };
                this.subscriptions.set(url, entry);
            }
            const subscribed = entry;
            const untyped = observer as SnapshotObserver<unknown>;
            entry.observers.add(untyped);
            if (entry.received) observer.next(entry.value as T);
            queueMicrotask(() => {
                this.ensureStream();
                this.pump();
            });
            return () => {
                subscribed.observers.delete(untyped);
                if (subscribed.observers.size) return;
                subscribed.pending?.abort();
                clearTimeout(subscribed.timer);
                if (this.subscriptions.get(url) === subscribed) this.subscriptions.delete(url);
                if (!this.subscriptions.size) this.stop();
            };
        });
    }

    private stop() {
        this.generation++;
        this.ready = false;
        // Retain ownership until the old reader finishes: never overlap SSE connections.
        this.stream?.abort();
        clearTimeout(this.retry);
        this.retry = undefined;
        for (const entry of this.subscriptions.values()) {
            entry.pending?.abort();
            clearTimeout(entry.timer);
            entry.timer = undefined;
        }
    }

    private failAll(error: Error) {
        const entries = [...this.subscriptions.values()];
        this.subscriptions.clear();
        for (const entry of entries) {
            entry.pending?.abort();
            clearTimeout(entry.timer);
            for (const observer of [...entry.observers]) observer.error(error);
        }
    }

    private renew() {
        this.refreshing ??= this.options
            .refresh()
            .then((success) => {
                this.sessionChanged();
                return success;
            })
            .finally(() => {
                this.refreshing = undefined;
            });
        return this.refreshing;
    }

    private ensureStream() {
        if (this.stream || this.retry || !this.subscriptions.size || !this.session.token) return;
        const controller = new AbortController();
        this.stream = controller;
        void this.listen(controller, this.session.token);
    }

    private async checkResponse(response: Response) {
        if (response.ok) return;
        throw new SnapshotError(
            `查询失败（HTTP ${response.status}）`,
            response.status < 500 && response.status !== 429,
            response.status === 401
        );
    }

    private async listen(controller: AbortController, token: string) {
        let timedOut = false;
        const timeout = setTimeout(() => {
            timedOut = true;
            controller.abort();
        }, 30000);
        try {
            const response = await this.fetch('/v1/auth/events', {
                headers: { Accept: 'text/event-stream', Authorization: `Bearer ${token}` },
                cache: 'no-store',
                credentials: 'same-origin',
                signal: controller.signal,
            });
            await this.checkResponse(response);
            if (
                !response.body ||
                !response.headers.get('content-type')?.includes('text/event-stream')
            )
                throw new SnapshotError('事件订阅格式无效', true);
            await consumeServerSentEvents(
                response.body,
                (event) => {
                    if (controller.signal.aborted || token !== this.session.token) return;
                    if (event.event === 'heartbeat') return;
                    const payload = JSON.parse(event.data);
                    if (event.event === 'error')
                        throw new SnapshotError(
                            payload.message ?? '订阅中断',
                            payload.code !== 10004,
                            payload.code === 11005
                        );
                    if (
                        !['ready', 'change'].includes(event.event) ||
                        !Array.isArray(payload.topics) ||
                        !payload.topics.every((topic: unknown) => typeof topic === 'string')
                    )
                        throw new SnapshotError('事件通知格式无效', true);
                    if (event.event === 'ready') {
                        clearTimeout(timeout);
                        this.ready = true;
                        this.backoff = 1000;
                    }
                    for (const entry of this.subscriptions.values()) {
                        if (
                            event.event === 'ready' ||
                            payload.topics.includes('*') ||
                            !entry.topic ||
                            payload.topics.includes(entry.topic)
                        ) {
                            entry.dirty = true;
                            if (event.event === 'ready') entry.nextRequest = 0;
                        }
                    }
                    this.pump();
                },
                controller.signal
            );
        } catch (failure) {
            if (!controller.signal.aborted) {
                const error = failure instanceof Error ? failure : new Error(String(failure));
                if (error instanceof SnapshotError && error.expired) {
                    let renewed = false;
                    try {
                        renewed = await this.renew();
                    } catch {
                        /* Report the authentication failure below. */
                    }
                    if (!renewed && token === this.session.token) this.failAll(error);
                } else if (error instanceof SnapshotError && error.terminal) this.failAll(error);
            }
        } finally {
            clearTimeout(timeout);
            this.ready = false;
            this.stream = undefined;
            if (this.subscriptions.size && this.session.token) {
                this.retry = setTimeout(
                    () => {
                        this.retry = undefined;
                        this.ensureStream();
                    },
                    controller.signal.aborted && !timedOut ? 0 : this.backoff
                );
                this.backoff = Math.min(15000, this.backoff * 2);
            }
        }
    }

    private pump() {
        if (!this.ready) return;
        for (const entry of this.subscriptions.values()) {
            if (this.running >= 3) break;
            if (!entry.dirty || entry.pending || entry.timer) continue;
            const delay = entry.nextRequest - Date.now();
            if (delay > 0) {
                entry.timer = setTimeout(() => {
                    entry.timer = undefined;
                    this.pump();
                }, delay);
                continue;
            }
            void this.readSnapshot(entry);
        }
    }

    private async readSnapshot(entry: Subscription) {
        const generation = this.generation;
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 30000);
        entry.pending = controller;
        entry.dirty = false;
        this.running++;
        const current = () =>
            generation === this.generation && this.subscriptions.get(entry.url) === entry;
        try {
            const response = await this.fetch(entry.url, {
                headers: {
                    Accept: 'application/json',
                    Authorization: `Bearer ${this.session.token}`,
                },
                cache: 'no-store',
                credentials: 'same-origin',
                signal: controller.signal,
            });
            await this.checkResponse(response);
            const payload = await response.json();
            if (!current()) return;
            if (controller.signal.aborted) throw new SnapshotError('查询超时');
            if (payload.code !== 0 || !('data' in payload))
                throw new SnapshotError(
                    payload.message ?? '查询快照格式无效',
                    payload.code !== 10004,
                    payload.code === 11005
                );
            entry.topic = response.headers.get('x-snapshot-topic') ?? undefined;
            entry.coalesce = Math.min(
                15000,
                Math.max(100, Number(response.headers.get('x-snapshot-coalesce-ms')) || 100)
            );
            entry.nextRequest = Date.now() + entry.coalesce;
            entry.value = payload.data;
            entry.received = true;
            for (const observer of [...entry.observers]) observer.next(payload.data);
        } catch (failure) {
            if (!current()) return;
            const error = failure instanceof Error ? failure : new Error(String(failure));
            if (error instanceof SnapshotError && error.expired) {
                let renewed = false;
                try {
                    renewed = await this.renew();
                } catch {
                    /* Clear expired subscriptions below. */
                }
                if (!current()) return;
                if (renewed) entry.dirty = true;
                else {
                    this.stop();
                    this.failAll(error);
                }
                return;
            }
            entry.received = false;
            entry.value = undefined;
            if (error instanceof SnapshotError && error.terminal)
                this.subscriptions.delete(entry.url);
            else {
                entry.dirty = true;
                entry.nextRequest = Date.now() + 1000;
            }
            for (const observer of [...entry.observers]) observer.error(error);
        } finally {
            clearTimeout(timeout);
            entry.pending = undefined;
            this.running--;
            if (!this.subscriptions.size) this.stop();
            this.pump();
        }
    }
}
