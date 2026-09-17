import { useAuthStore } from '@/store/authStore';
import { refreshSession } from './http';
import { SseSubscriptions } from './sse-subscriptions';

const sseSubscriptions = new SseSubscriptions({
    session: () => {
        const { token, user } = useAuthStore.getState();
        return { token, userId: user?.id ?? null };
    },
    refresh: refreshSession,
    fetch: (input, init) => fetch(input, init),
});
useAuthStore.subscribe(() => sseSubscriptions.sessionChanged());

export function createSseSnapshotStream<T>(
    path: string,
    params?: Record<string, unknown>,
    eventName = 'snapshot'
) {
    return sseSubscriptions.create<T>(path, params, eventName);
}

export function createSharedSseSnapshotStream<T>(eventName: string) {
    return sseSubscriptions.createShared<T>(eventName);
}
