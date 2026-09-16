// HTTP/1 browsers have a small per-origin connection pool. Keep ordinary requests
// from waiting behind persistent SSE responses, including during token refresh.
let pendingRequests = 0;
const listeners = new Set<(pending: boolean) => void>();

export async function trackHttpRequest<T>(send: () => Promise<T>): Promise<T> {
    pendingRequests++;
    if (pendingRequests === 1) {
        for (const listener of listeners) listener(true);
    }
    try {
        return await send();
    } finally {
        pendingRequests--;
        if (pendingRequests === 0) {
            for (const listener of listeners) listener(false);
        }
    }
}

export function subscribeHttpActivity(listener: (pending: boolean) => void) {
    listeners.add(listener);
    listener(pendingRequests > 0);
    return () => {
        listeners.delete(listener);
    };
}

export function waitForHttpIdle(signal: AbortSignal): Promise<void> {
    if (!pendingRequests || signal.aborted) return Promise.resolve();
    return new Promise((resolve) => {
        const finish = () => {
            listeners.delete(onActivity);
            signal.removeEventListener('abort', finish);
            resolve();
        };
        const onActivity = (pending: boolean) => {
            if (!pending) finish();
        };
        listeners.add(onActivity);
        signal.addEventListener('abort', finish, { once: true });
    });
}
