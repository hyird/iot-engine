import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import type { PaginatedResult } from '@/utils/types';
import { create, getEnums, getList, getPublicIp, remove, update } from './link.api';
import { type Link, linkQueryKeys } from './link.types';

export function useLinkList(
    params?: Link.Query,
    options?: Omit<UseQueryOptions<PaginatedResult<Link.Item>>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: linkQueryKeys.list(params),
        queryFn: () => getList(params),
        ...options,
    });
}

export function useLinkEnums(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: [...linkQueryKeys.all, 'enums'],
        queryFn: getEnums,
        enabled: options?.enabled ?? true,
        staleTime: Number.POSITIVE_INFINITY,
    });
}

export function usePublicIp(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: [...linkQueryKeys.all, 'public-ip'],
        queryFn: getPublicIp,
        enabled: options?.enabled ?? true,
        staleTime: 5 * 60 * 1000,
        retry: false,
    });
}

export function useLinkOptions(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: [...linkQueryKeys.all, 'options'],
        queryFn: async () => (await getList({ page: 1, pageSize: 100 })).list,
        enabled: options?.enabled ?? true,
    });
}

export function useLinkSave() {
    return useSaveMutation<Link.SaveDto & { id?: string }, Link.SaveDto, Link.SaveDto>({
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
