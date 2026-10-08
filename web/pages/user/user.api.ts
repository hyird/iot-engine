import request from '@/lib/http';
import type { PaginatedResult } from '@/types/pagination';
import {
    createUserSchema,
    updateUserSchema,
    userIdSchema,
    userListQuerySchema,
    userOptionsQuerySchema,
} from './user.schema';
import type { User } from './user.types';

export function getList(params?: User.Query, signal?: AbortSignal) {
    const query = userListQuerySchema.parse(params ?? {});
    return request.get<PaginatedResult<User.Item>>('/v1/users', { params: query, signal });
}
export function getOptions(params?: Pick<User.Query, 'keyword'>, signal?: AbortSignal) {
    const query = userOptionsQuerySchema.parse(params ?? {});
    return request.get<User.Option[]>('/v1/users/options', { params: query, signal });
}
export function create(data: User.CreateDto) {
    return request.post<void>('/v1/users', createUserSchema.parse(data));
}
export function update(id: string, data: User.UpdateDto) {
    const validatedId = userIdSchema.parse(id);
    return request.put<void>(`/v1/users/${validatedId}`, updateUserSchema.parse(data));
}
export function remove(id: string) {
    return request.delete<void>(`/v1/users/${userIdSchema.parse(id)}`);
}
