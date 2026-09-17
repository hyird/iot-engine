import { useQuery, type UseQueryOptions } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import type { PaginatedResult } from '@/types/pagination';
import { create, getList, getOptions, remove, update } from './role.api';
import type { Role } from './role.types';
import { roleQueryKeys } from './role.types';
export const getRoleOptions = getOptions;
export function useRoleList(
    params?: Role.Query,
    options?: Omit<UseQueryOptions<PaginatedResult<Role.Item>>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: roleQueryKeys.list(params),
        queryFn: ({ signal }) => getList(params, signal),
        ...options,
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useRoleSave() {
    return useSaveMutation<
        Role.CreateDto & {
            id?: string;
        },
        Role.CreateDto,
        Role.UpdateDto
    >({
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
