import { useQuery, type UseQueryOptions } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import type { PaginatedResult } from '@/types/pagination';
import { create, getList, getOptions, remove, update } from './dept.api';
import type { Dept } from './dept.types';
import { deptQueryKeys } from './dept.types';
export function useDeptList(
    params?: Dept.Query,
    options?: Omit<UseQueryOptions<PaginatedResult<Dept.Item>>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: deptQueryKeys.list(params),
        queryFn: ({ signal }) => getList(params, signal),
        ...options,
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useDeptOptions(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: deptQueryKeys.options(),
        queryFn: ({ signal }) => getOptions(signal),
        enabled: options?.enabled ?? true,
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useDeptSave() {
    return useSaveMutation<
        Dept.CreateDto & {
            id?: string;
        },
        Dept.CreateDto,
        Dept.UpdateDto
    >({
        createFn: create,
        updateFn: update,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '保存成功',
        updateMessage: '保存成功',
        invalidateKeys: [deptQueryKeys.all],
    });
}
export function useDeptDelete() {
    return useMutationWithMessage({
        mutationFn: remove,
        successMessage: '删除成功',
        invalidateKeys: [deptQueryKeys.all],
    });
}
