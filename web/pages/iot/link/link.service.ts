import { retainNewerRecords } from '@/utils/versioned-records';
import type { UseQueryOptions } from '@tanstack/react-query';
import { setDebug as saveDebugSwitch, getDebugPackets } from './link.api';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import type { PaginatedResult } from '@/types/pagination';
import { create, getEnums, getList, getPublicIp, remove, update } from './link.api';
import type { Link } from './link.types';
import { linkQueryKeys } from './link.types';
export function useLinkList(
    params?: Link.Query,
    options?: Omit<UseQueryOptions<PaginatedResult<Link.Item>>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: linkQueryKeys.list(params),
        queryFn: () => getList(params),
        ...options,
    });
}
export function useLinkEnums(options?: { enabled?: boolean }) {
    return useSnapshotQuery({
        queryKey: [...linkQueryKeys.all, 'enums'],
        queryFn: getEnums,
        enabled: options?.enabled ?? true,
        staleTime: Number.POSITIVE_INFINITY,
    });
}
export function usePublicIp(options?: { enabled?: boolean }) {
    return useSnapshotQuery({
        queryKey: [...linkQueryKeys.all, 'public-ip'],
        queryFn: getPublicIp,
        enabled: options?.enabled ?? true,
        staleTime: 5 * 60 * 1000,
        retry: false,
    });
}
export function useLinkOptions(
    options?: Omit<UseQueryOptions<Link.Item[]>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: [...linkQueryKeys.all, 'options'],
        queryFn: () => getList({ page: 1, pageSize: 100 }).map((page) => page.list),
        ...options,
        enabled: options?.enabled ?? true,
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

export function useLinkDebug(id: string, open: boolean) {
    const packets = useSnapshotQuery({
        queryKey: ['link-debug-packets', id],
        queryFn: () => getDebugPackets(id),
        structuralSharing: retainNewerRecords,
        enabled: open,
    });
    const toggle = useMutationWithMessage({
        mutationFn: (enabled: boolean) => saveDebugSwitch(id, enabled),
        successMessage: '调试设置已保存',
        invalidateKeys: [linkQueryKeys.all],
    });
    return { packets, toggle };
}
