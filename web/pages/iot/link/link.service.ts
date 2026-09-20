import { createQueryKeys } from '@/utils/query';
import { retainNewerRecords } from '@/utils/versioned-records';
import { keepPreviousData, useQuery, type UseQueryOptions } from '@tanstack/react-query';
import { setDebug as saveDebugSwitch, getDebugPackets } from './link.api';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import type { PaginatedResult } from '@/types/pagination';
import { create, getEnums, getList, queryList, getPublicIp, remove, update } from './link.api';
import type { Link } from './link.types';
const keys = createQueryKeys('links');
const linkQueryKeys = {
    ...keys,
    list: (params?: Link.Query) => [...keys.lists(), params] as const,
};

export function useLinkList(
    params?: Link.Query,
    options?: Omit<UseQueryOptions<PaginatedResult<Link.Item>>, 'queryKey' | 'queryFn'>,
    debugLinkId?: string
) {
    return useSnapshotQuery({
        queryKey: [...linkQueryKeys.list(params), debugLinkId],
        queryFn: () => getList(params, debugLinkId),
        placeholderData: keepPreviousData,
        ...options,
    });
}
export function useLinkEnums(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: [...linkQueryKeys.all, 'enums'],
        queryFn: ({ signal }) => getEnums(signal),
        enabled: options?.enabled ?? true,
        staleTime: Number.POSITIVE_INFINITY,
        refetchInterval: false,
    });
}
export function usePublicIp(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: [...linkQueryKeys.all, 'public-ip'],
        queryFn: ({ signal }) => getPublicIp(signal),
        enabled: options?.enabled ?? true,
        staleTime: 5 * 60 * 1000,
        retry: false,
        refetchInterval: false,
    });
}
export function useLinkOptions(
    options?: Omit<UseQueryOptions<Link.Item[]>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: [...linkQueryKeys.all, 'options'],
        queryFn: async ({ signal }) => {
            const first = await queryList({ page: 1, pageSize: 100 }, signal);
            const links = new Map(first.list.map((link) => [link.id, link]));
            for (let page = 2; page <= Math.ceil(first.total / 100); page++) {
                const result = await queryList({ page, pageSize: 100 }, signal);
                for (const link of result.list) links.set(link.id, link);
            }
            return [...links.values()];
        },
        ...options,
        enabled: options?.enabled ?? true,
        refetchInterval: false,
    });
}
export function useLinkSave() {
    return useSaveMutation<
        Link.SaveDto & {
            id?: string;
        },
        Link.SaveDto,
        Link.SaveDto
    >({
        createFn: create,
        updateFn: update,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '保存成功',
        updateMessage: '保存成功',
        invalidateKeys: [linkQueryKeys.all],
    });
}
export function useLinkDelete() {
    return useMutationWithMessage({
        mutationFn: remove,
        successMessage: '删除成功',
        invalidateKeys: [linkQueryKeys.all],
    });
}

export function useLinkDebug(id: string, open: boolean, params?: Link.Query) {
    const packets = useSnapshotQuery({
        queryKey: ['link-debug-packets', id, params],
        queryFn: () => getDebugPackets(id, params),
        structuralSharing: retainNewerRecords,
        enabled: open,
    });
    const toggle = useMutationWithMessage({
        mutationFn: (enabled: boolean) => saveDebugSwitch(id, enabled),
        successMessage: '调试设置已保存',
    });
    return { packets, toggle };
}
