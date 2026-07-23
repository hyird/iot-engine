import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import type { PaginatedResult } from '@/utils/types';
import { create, getList, remove, update } from './role.client';
import { roleQueryKeys, type Role } from './role.types';

export function useRoleList(
    params?: Role.Query,
    options?: Omit<UseQueryOptions<PaginatedResult<Role.Item>>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: roleQueryKeys.list(params),
        queryFn: () => getList(params),
        ...options,
    });
}

export function useRoleSave() {
    return useSaveMutation<Role.CreateDto & { id?: string }, Role.CreateDto, Role.UpdateDto>({
        createFn: create,
        updateFn: update,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '保存成功',
        updateMessage: '保存成功',
        invalidateKeys: [roleQueryKeys.all, ['roles', 'options']],
    });
}

export function useRoleDelete() {
    return useMutationWithMessage({
        mutationFn: remove,
        successMessage: '删除成功',
        invalidateKeys: [roleQueryKeys.all, ['roles', 'options']],
    });
}
