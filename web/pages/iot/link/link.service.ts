import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import type { PaginatedResult } from '@/utils/types';
import { create, getList, remove, update } from './link.api';
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

export function useLinkSave() {
    return useSaveMutation<Link.SaveDto & { id?: number }, Link.SaveDto, Link.SaveDto>({
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
