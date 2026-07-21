/**
 * 用户管理 API
 */

import type { User } from './user.types';
import {
    createUserSchema,
    updateUserSchema,
    userIdSchema,
    userListQuerySchema,
    userOptionsQuerySchema,
} from './user.schema';
import type { PaginatedResult } from '@/utils/types';
import { appendQueryParams } from '@/utils/query';
import request from '@/utils/http';

const ENDPOINTS = {
    BASE: '/api/users',
    DETAIL: (id: string) => `/api/users/${id}`,
    OPTIONS: '/api/users/options',
} as const;

export function getList(params?: User.Query) {
    const query = userListQuerySchema.parse(params ?? {});
    return request.get<PaginatedResult<User.Item>>(appendQueryParams(ENDPOINTS.BASE, query));
}

export function getOptions(params?: Pick<User.Query, 'keyword'>) {
    const query = userOptionsQuerySchema.parse(params ?? {});
    return request.get<User.Option[]>(appendQueryParams(ENDPOINTS.OPTIONS, query));
}

export function getRoleOptions() {
    return request.get<User.Role[]>('/api/roles/options');
}

export function create(data: User.CreateDto) {
    return request.post<void>(ENDPOINTS.BASE, createUserSchema.parse(data));
}

export function update(id: string, data: User.UpdateDto) {
    const validatedId = userIdSchema.parse(id);
    return request.put<void>(ENDPOINTS.DETAIL(validatedId), updateUserSchema.parse(data));
}

export function remove(id: string) {
    return request.delete<void>(ENDPOINTS.DETAIL(userIdSchema.parse(id)));
}
