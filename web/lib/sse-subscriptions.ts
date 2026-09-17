import { HttpRequestError, queryUrl } from './http';
import { type SnapshotObserver, SnapshotStream } from './snapshot-stream';
import { consumeServerSentEvents } from './sse';

interface Session {
    token: string | null;
    userId: string | null;
}
interface Subscription {
    url: string;
    observers: Map<SnapshotObserver<unknown>, string>;
    controller?: AbortController;
    failure?: Error;
    values: Map<string, unknown>;
    errors: Map<string, Error>;
}

/** Shared subscriptions are scoped to the current account and actual observers. */
export class SseSubscriptions {
    private readonly subscriptions = new Map<string, Subscription>();
    private readonly receivers = new Map<AbortController, Promise<void>>();
    private session: Session;
    private sharedSource?: { event: string };
    private readonly sharedObservers = new Set<SnapshotObserver<unknown>>();
    private sharedValue: unknown;
    private sharedReceived = false;
    private sharedError?: Error;

    constructor(
        private readonly options: {
            session: () => Session;
            refresh: () => Promise<boolean>;
            fetch: typeof fetch;
            retryDelay?: (attempt: number, signal: AbortSignal) => Promise<void>;
        }
    ) {
        this.session = options.session();
    }

    sessionChanged() {
        const next = this.options.session();
        if (next.token === this.session.token && next.userId === this.session.userId) return;
        const changedAccount = !next.token || next.userId !== this.session.userId;
        this.session = next;
        this.sharedReceived = false;
        this.sharedValue = undefined;
        this.sharedError = undefined;
        if (changedAccount) {
            for (const observer of this.sharedObservers)
                this.reportError(observer, new Error('登录状态已变更'));
            this.sharedObservers.clear();
        }
        for (const entry of [...this.subscriptions.values()]) {
            entry.controller?.abort();
            entry.controller = undefined;
            entry.values.clear();
            entry.errors.clear();
            if (changedAccount) {
                this.fail(entry, new Error('登录状态已变更'));
                if (this.subscriptions.get(entry.url) === entry)
                    this.subscriptions.delete(entry.url);
                entry.observers.clear();
            } else {
                entry.failure = undefined;
                this.start(entry);
            }
        }
        queueMicrotask(() => this.syncShared());
    }

    createShared<T>(eventName: string): SnapshotStream<T> {
        if (this.sharedSource && this.sharedSource.event !== eventName)
            throw new Error('共享订阅来源不能重复配置');
        if (!this.sharedSource) {
            this.sharedSource = { event: eventName };
            for (const entry of this.subscriptions.values()) {
                entry.controller?.abort();
                entry.controller = undefined;
                entry.values.clear();
                entry.errors.clear();
                queueMicrotask(() => this.start(entry));
            }
        }
        return new SnapshotStream((observer) => {
            this.sessionChanged();
            if (!this.session.token) {
                observer.error(new Error('登录状态无效'));
                return () => {};
            }
            const untyped: SnapshotObserver<unknown> = {
                next: (value) => observer.next(value as T),
                error: (error) => observer.error(error),
            };
            this.sharedObservers.add(untyped);
            if (this.sharedError) observer.error(this.sharedError);
            else if (this.sharedReceived) observer.next(this.sharedValue as T);
            queueMicrotask(() => this.syncShared());
            return () => {
                this.sharedObservers.delete(untyped);
                queueMicrotask(() => this.syncShared());
            };
        });
    }

    private primarySubscription() {
        return [...this.subscriptions.values()].find(
            (entry) => entry.observers.size && !entry.failure
        );
    }

    private syncShared() {
        if (!this.sharedSource) return;
        const primary = this.primarySubscription();
        if (!primary) {
            this.sharedReceived = false;
            this.sharedValue = undefined;
            this.sharedError = undefined;
            return;
        }
        if (!this.sharedObservers.size || !this.session.token) return;
        const error = primary.errors.get(this.sharedSource.event);
        if (error) this.publishSharedError(error);
        else if (primary.values.has(this.sharedSource.event))
            this.publishShared(primary.values.get(this.sharedSource.event));
    }

    private publishShared(value: unknown) {
        if (this.sharedReceived && !this.sharedError && this.sharedValue === value) return;
        this.sharedValue = value;
        this.sharedReceived = true;
        this.sharedError = undefined;
        for (const observer of [...this.sharedObservers]) {
            try {
                observer.next(value);
            } catch (error) {
                this.reportError(
                    observer,
                    error instanceof Error ? error : new Error(String(error))
                );
            }
        }
    }

    private publishSharedError(error: Error) {
        if (this.sharedError === error) return;
        this.sharedError = error;
        this.sharedReceived = false;
        this.sharedValue = undefined;
        for (const observer of [...this.sharedObservers]) this.reportError(observer, error);
    }

    create<T>(
        path: string,
        params?: Record<string, unknown>,
        eventName = 'snapshot'
    ): SnapshotStream<T> {
        const url = queryUrl(path, params);
        return new SnapshotStream((observer, options) => {
            this.sessionChanged();
            if (!this.session.token || (!path.startsWith('/v1/') && !path.startsWith('/api/'))) {
                observer.error(new Error('登录状态或订阅地址无效'));
                return () => {};
            }
            let entry = this.subscriptions.get(url);
            if (!entry) {
                entry = { url, observers: new Map(), values: new Map(), errors: new Map() };
                this.subscriptions.set(url, entry);
            }
            const subscribed = entry;
            if (options?.fresh && (entry.failure || entry.errors.has(eventName))) {
                entry.controller?.abort();
                entry.controller = undefined;
                entry.values.clear();
                entry.errors.clear();
                entry.failure = undefined;
            }
            const untyped: SnapshotObserver<unknown> = {
                next: (value) => observer.next(value as T),
                error: (error) => observer.error(error),
            };
            entry.observers.set(untyped, eventName);
            const error = entry.failure ?? entry.errors.get(eventName);
            if (error) observer.error(error);
            else if (entry.values.has(eventName)) observer.next(entry.values.get(eventName) as T);
            queueMicrotask(() => {
                this.syncShared();
                this.start(subscribed);
            });
            return () => {
                subscribed.observers.delete(untyped);
                if (subscribed.observers.size || this.subscriptions.get(url) !== subscribed) return;
                this.subscriptions.delete(url);
                subscribed.controller?.abort();
                queueMicrotask(() => this.syncShared());
            };
        });
    }

    private start(entry: Subscription) {
        if (
            entry.controller ||
            entry.failure ||
            this.subscriptions.get(entry.url) !== entry ||
            !entry.observers.size
        )
            return;
        const controller = new AbortController();
        entry.controller = controller;
        const closing = [...this.receivers]
            .filter(([previous]) => previous.signal.aborted)
            .map(([, finished]) => finished);
        const finished = Promise.all(closing)
            .then(() => {
                if (!controller.signal.aborted) return this.receive(entry, controller);
            })
            .finally(() => this.receivers.delete(controller));
        this.receivers.set(controller, finished);
    }

    private async receive(entry: Subscription, controller: AbortController) {
        const { signal } = controller;
        let failures = 0;
        let refreshed = false;
        while (!signal.aborted) {
            const connectedAt = Date.now();
            try {
                const response = await this.options.fetch(entry.url, {
                    headers: {
                        Accept: 'text/event-stream',
                        Authorization: `Bearer ${this.session.token}`,
                        ...(this.sharedSource ? { 'X-SSE-User': '1' } : {}),
                    },
                    cache: 'no-store',
                    signal,
                });
                if (signal.aborted) {
                    await response.body?.cancel();
                    return;
                }
                if (!response.ok) {
                    const error = await response.json().catch(() => ({}));
                    throw new HttpRequestError(
                        error.message ?? '实时订阅连接失败',
                        response.status,
                        error.code
                    );
                }
                if (
                    !response.body ||
                    !response.headers.get('content-type')?.includes('text/event-stream')
                )
                    throw new HttpRequestError('服务器未返回 SSE 数据流', 406);
                await consumeServerSentEvents(
                    response.body,
                    (event) => {
                        if (signal.aborted || event.event === 'heartbeat') return;
                        const result = JSON.parse(event.data);
                        if (result.code !== 0 || event.event === 'error') {
                            const error = new HttpRequestError(
                                result.message ?? '实时订阅已中断',
                                result.code === 11005 ? 401 : result.code === 10004 ? 500 : 403,
                                result.code
                            );
                            if (event.event === 'error' || event.event === 'snapshot') throw error;
                            entry.values.delete(event.event);
                            entry.errors.set(event.event, error);
                            if (
                                event.event === this.sharedSource?.event &&
                                entry === this.primarySubscription()
                            )
                                this.publishSharedError(error);
                            for (const [observer, name] of [...entry.observers]) {
                                if (name === event.event) this.reportError(observer, error);
                            }
                            return;
                        }
                        entry.errors.delete(event.event);
                        entry.values.set(event.event, result.data);
                        if (
                            event.event === this.sharedSource?.event &&
                            entry === this.primarySubscription()
                        )
                            this.publishShared(result.data);
                        for (const [observer, name] of [...entry.observers]) {
                            if (name !== event.event) continue;
                            try {
                                observer.next(result.data);
                            } catch (error) {
                                entry.observers.delete(observer);
                                this.reportError(
                                    observer,
                                    error instanceof Error ? error : new Error(String(error))
                                );
                            }
                        }
                        if (!entry.observers.size) {
                            this.subscriptions.delete(entry.url);
                            controller.abort();
                        }
                    },
                    signal
                );
                if (signal.aborted) return;
                throw new Error('实时连接已断开');
            } catch (failure) {
                if (signal.aborted) return;
                const error = failure instanceof Error ? failure : new Error(String(failure));
                if (error instanceof HttpRequestError && error.status === 401 && !refreshed) {
                    refreshed = true;
                    if (await this.options.refresh().catch(() => false)) {
                        this.sessionChanged();
                        if (!signal.aborted) continue;
                        return;
                    }
                }
                if (
                    (error instanceof HttpRequestError && error.status < 500) ||
                    error instanceof SyntaxError
                ) {
                    this.fail(entry, error);
                    return;
                }
                // Reconnect only after a failed connection, never on a healthy-stream timer.
                if (Date.now() - connectedAt >= 30000) failures = 0;
                if (++failures > 8) {
                    this.fail(entry, error);
                    return;
                }
                try {
                    await (this.options.retryDelay ?? waitBeforeReconnect)(failures, signal);
                } catch {
                    if (!signal.aborted) this.fail(entry, error);
                    return;
                }
            }
        }
    }

    private fail(entry: Subscription, error: Error) {
        if (this.subscriptions.get(entry.url) !== entry) return;
        entry.controller?.abort();
        entry.controller = undefined;
        entry.failure = error;
        entry.values.clear();
        entry.errors.clear();
        for (const observer of [...entry.observers.keys()]) this.reportError(observer, error);
        queueMicrotask(() => this.syncShared());
    }

    private reportError(observer: SnapshotObserver<unknown>, error: Error) {
        try {
            observer.error(error);
        } catch {
            /* Other subscribers remain independent. */
        }
    }
}

function waitBeforeReconnect(attempt: number, signal: AbortSignal): Promise<void> {
    return new Promise((resolve, reject) => {
        const abort = () => {
            clearTimeout(timer);
            signal.removeEventListener('abort', abort);
            reject(signal.reason);
        };
        const timer = setTimeout(
            () => {
                signal.removeEventListener('abort', abort);
                resolve();
            },
            Math.min(1000 * 2 ** (attempt - 1), 30000)
        );
        signal.addEventListener('abort', abort, { once: true });
        if (signal.aborted) abort();
    });
}
