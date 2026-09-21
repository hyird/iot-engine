import { hashKey, type UseQueryOptions, useQuery, useQueryClient } from '@tanstack/react-query';
import { useEffect, useRef } from 'react';
import type { SnapshotStream } from '@/lib/snapshot-stream';
import { useAuthStore } from '@/store/authStore';

type Options<T, Selected> = Omit<UseQueryOptions<T, Error, Selected>, 'queryFn'> & {
    queryFn: () => SnapshotStream<T>;
    streamKey?: unknown;
};

export function useSnapshotQuery<T, Selected = T>(options: Options<T, Selected>) {
    const client = useQueryClient();
    const token = useAuthStore((state) => state.token);
    const factory = useRef(options.queryFn);
    factory.current = options.queryFn;
    const key = hashKey(options.queryKey);
    const streamKey = hashKey([options.streamKey ?? options.queryKey]);
    const result = useQuery({
        ...options,
        queryFn: ({ signal }) => factory.current().first(signal, { fresh: true }),
        staleTime: Infinity,
        retry: false,
        refetchInterval: false,
        refetchOnWindowFocus: false,
        refetchOnReconnect: false,
    });
    const enabled = options.enabled !== false;
    useEffect(() => {
        if (!enabled || !token) return;
        void streamKey;
        const queryKey = JSON.parse(key);
        return factory.current().subscribe({
            next: (value) => client.setQueryData(queryKey, value),
            error: (error) => {
                const query = client.getQueryCache().find({ queryKey, exact: true });
                query?.setState({ data: undefined, error, status: 'error', fetchStatus: 'idle' });
            },
        });
    }, [client, key, streamKey, enabled, token]);
    return result;
}
