/**
 * 用户管理 Service
 */

import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import type { User } from './user.types';
import { roleOptionQueryKey, userQueryKeys } from './user.types';
import type { PaginatedResult } from '@/utils/types';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { create, getDetail, getList, getOptions, getRoleOptions, remove, update } from './user.api';

export { create, getDetail, getList, getOptions, remove, update } from './user.api';

// ============ Queries ============

type UserListResult = PaginatedResult<User.Item>;

export function useUserList(
    params?: User.Query,
    options?: Omit<UseQueryOptions<UserListResult>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: userQueryKeys.list(params),
        queryFn: () => getList(params),
        ...options,
    });
}

export function useUserDetail(
    id: number,
    options?: Omit<UseQueryOptions<User.Item>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: userQueryKeys.detail(id),
        queryFn: () => getDetail(id),
        enabled: !!id,
        ...options,
    });
}

export function useUserOptions(
    options?: Omit<UseQueryOptions<User.Option[]>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: userQueryKeys.options(),
        queryFn: () => getOptions(),
        staleTime: 5 * 60 * 1000,
        ...options,
    });
}

export function useRoleOptions(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: roleOptionQueryKey,
        queryFn: getRoleOptions,
        staleTime: 5 * 60 * 1000,
        enabled: options?.enabled ?? true,
    });
}

// ============ Mutations ============

export function useUserDelete() {
    return useMutationWithMessage({
        mutationFn: remove,
        successMessage: '删除成功',
        invalidateKeys: [userQueryKeys.all],
    });
}

export function useUserSave() {
    return useSaveMutation<User.CreateDto & { id?: number }, User.CreateDto, User.UpdateDto>({
        createFn: create,
        updateFn: update,
        toUpdatePayload: ({
            id: _id,
            username: _username,
            ...data
        }: User.CreateDto & { id?: number }) => data as User.UpdateDto,
        createMessage: '保存成功',
        updateMessage: '保存成功',
        invalidateKeys: [userQueryKeys.all],
    });
}
