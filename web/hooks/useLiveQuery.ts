import { hashKey, useQuery, useQueryClient, type UseQueryOptions } from '@tanstack/react-query';
import { useEffect, useRef } from 'react';
import { useAuthStore } from '@/store/authStore';
import type { LiveResource } from '@/utils/live-resource';

type Options<T, Selected> = Omit<UseQueryOptions<T, Error, Selected>, 'queryFn'> & {
    queryFn: () => LiveResource<T>;
};

export function useLiveQuery<T, Selected = T>(options: Options<T, Selected>) {
    const client = useQueryClient();
    const token = useAuthStore((state) => state.token);
    const factory = useRef(options.queryFn);
    factory.current = options.queryFn;
    const key = hashKey(options.queryKey);
    const result = useQuery({
        ...options,
        queryFn: ({ signal }) => factory.current().first(signal),
        staleTime: Infinity,
        refetchInterval: false,
        refetchOnWindowFocus: false,
        refetchOnReconnect: false,
    });
    const enabled = options.enabled !== false;
    useEffect(() => {
        if (!enabled || !token) return;
        const queryKey = JSON.parse(key);
        return factory.current().subscribe({
            next: (value) => client.setQueryData(queryKey, value),
            error: (error) => {
                const query = client.getQueryCache().find({ queryKey, exact: true });
                query?.setState({ data: undefined, error, status: 'error', fetchStatus: 'idle' });
            },
        });
    }, [client, key, enabled, token]);
    return result;
}
