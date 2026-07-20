/**
 * React Query 工具
 */

import { QueryClient } from '@tanstack/react-query';

export const queryClient = new QueryClient({
    defaultOptions: {
        queries: {
            staleTime: 5 * 60 * 1000,
            gcTime: 10 * 60 * 1000,
            refetchOnWindowFocus: false,
            refetchOnReconnect: true,
            retry: 1,
        },
        mutations: {
            retry: 0,
        },
    },
});

export const deviceKeys = {
    all: ['device'] as const,
    realtime: () => ['device', 'realtime'] as const,
};

export function createQueryKeys<T extends string>(module: T) {
    return {
        all: [module] as const,
        lists: () => [module, 'list'] as const,
        list: <P extends Record<string, unknown>>(params: P) => [module, 'list', params] as const,
        details: () => [module, 'detail'] as const,
        detail: (id: number | string) => [module, 'detail', id] as const,
        trees: () => [module, 'tree'] as const,
        tree: <P extends Record<string, unknown>>(params?: P) => [module, 'tree', params] as const,
        options: () => [module, 'options'] as const,
    };
}

export function appendQueryParams<T extends object>(url: string, params?: T) {
    if (!params) {
        return url;
    }

    const searchParams = new URLSearchParams();

    Object.entries(params as Record<string, unknown>).forEach(([key, value]) => {
        if (value === undefined || value === null || value === '') {
            return;
        }
        searchParams.set(key, String(value));
    });

    const queryString = searchParams.toString();
    return queryString ? `${url}?${queryString}` : url;
}
