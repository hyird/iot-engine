import { useAuthStore } from '@/store/authStore';
import { refreshSession } from './http';
import { SnapshotSubscriptions } from './snapshot-subscriptions';

const subscriptions = new SnapshotSubscriptions({
    session: () => {
        const { token, user } = useAuthStore.getState();
        return { token, userId: user?.id ?? null };
    },
    refresh: refreshSession,
});
useAuthStore.subscribe(() => subscriptions.sessionChanged());

export function createSnapshotStream<T>(url: string, _options?: { _silent?: boolean }) {
    return subscriptions.create<T>(url);
}
