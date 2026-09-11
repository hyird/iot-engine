import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import type { UseQueryOptions } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import type { PaginatedResult } from '@/utils/pagination';
import { create, getList, getOptions, remove, update } from './dept.api';
import { deptQueryKeys, type Dept } from './dept.types';

export function useDeptList(
    params?: Dept.Query,
    options?: Omit<UseQueryOptions<PaginatedResult<Dept.Item>>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: deptQueryKeys.list(params),
        queryFn: () => getList(params),
        ...options,
    });
}
export function useDeptOptions(options?: { enabled?: boolean }) {
    return useSnapshotQuery({
        queryKey: deptQueryKeys.options(),
        queryFn: getOptions,
        enabled: options?.enabled ?? true,
    });
}
export function useDeptSave() {
    return useSaveMutation<Dept.CreateDto & { id?: string }, Dept.CreateDto, Dept.UpdateDto>({
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
