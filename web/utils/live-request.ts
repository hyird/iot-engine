import { useAuthStore } from '@/store/authStore';
import { consumeServerSentEvents } from './sse';
import { LiveResource, type LiveObserver } from './live-resource';

class SubscriptionError extends Error {
    constructor(message: string, readonly terminal: boolean) { super(message); }
}

interface Connection {
    controller: AbortController;
    observers: Set<LiveObserver<unknown>>;
    value?: unknown;
    received: boolean;
    terminalError?: Error;
}

const connections = new Map<string, Connection>();
let refresh: Promise<boolean> | undefined;

function delay(ms: number, signal: AbortSignal) {
    return new Promise<void>((resolve) => {
        const done = () => { clearTimeout(timer); signal.removeEventListener('abort', done); resolve(); };
        const timer = setTimeout(done, ms);
        signal.addEventListener('abort', done, { once: true });
        if (signal.aborted) done();
    });
}

async function run(url: string, connection: Connection) {
    const { signal } = connection.controller;
    let retry = 1000;
    while (!signal.aborted) {
        try {
            const token = useAuthStore.getState().token;
            if (!token) throw new SubscriptionError('登录状态已失效', true);
            const response = await fetch(url, {
                headers: { Accept: 'text/event-stream', Authorization: `Bearer ${token}` },
                cache: 'no-store', credentials: 'same-origin', signal,
            });
            if (response.status === 401) {
                refresh ??= useAuthStore.getState().refreshAccessToken().finally(() => { refresh = undefined; });
                if (await refresh) continue;
                throw new SubscriptionError('登录状态已失效', true);
            }
            if (!response.ok) throw new SubscriptionError(`订阅失败（HTTP ${response.status}）`, response.status < 500);
            if (!response.body || !response.headers.get('content-type')?.includes('text/event-stream'))
                throw new SubscriptionError('查询接口必须返回 SSE', true);
            await consumeServerSentEvents(response.body, (event) => {
                if (event.event === 'heartbeat') return;
                const payload = JSON.parse(event.data);
                if (event.event === 'error')
                    throw new SubscriptionError(payload.message ?? '订阅中断', payload.code !== 10004 && payload.code !== 11005);
                if (event.event !== 'snapshot' || payload.code !== 0 || !('data' in payload))
                    throw new SubscriptionError('查询快照格式无效', true);
                retry = 1000;
                connection.value = payload.data;
                connection.received = true;
                for (const observer of connection.observers) observer.next(payload.data);
            }, signal);
        } catch (failure) {
            if (signal.aborted) return;
            const error = failure instanceof Error ? failure : new Error(String(failure));
            connection.received = false;
            connection.value = undefined;
            const terminal = error instanceof SubscriptionError && error.terminal;
            if (terminal) connection.terminalError = error;
            for (const observer of connection.observers) observer.error(error);
            if (terminal) return;
        }
        await delay(retry, signal);
        retry = Math.min(15000, retry * 2);
    }
}

export function liveRead<T>(url: string, _options?: { _silent?: boolean }): LiveResource<T> {
    return new LiveResource<T>((observer) => {
        const key = `${useAuthStore.getState().token ?? ''}\n${url}`;
        let connection = connections.get(key);
        if (connection?.terminalError) connection = undefined;
        if (!connection) {
            connection = { controller: new AbortController(), observers: new Set(), received: false };
            connections.set(key, connection);
            // Register observers before fetch can synchronously fail.
            queueMicrotask(() => { if (!connection!.controller.signal.aborted) void run(url, connection!); });
        }
        const untyped = observer as LiveObserver<unknown>;
        connection.observers.add(untyped);
        if (connection.received) observer.next(connection.value as T);
        return () => {
            connection!.observers.delete(untyped);
            if (connection!.observers.size === 0) {
                connection!.controller.abort();
                if (connections.get(key) === connection) connections.delete(key);
            }
        };
    });
}
