import request from '@/lib/http';
import type { PaginatedResult } from '@/types/pagination';
import {
    createRoleSchema,
    roleIdSchema,
    roleListQuerySchema,
    updateRoleSchema,
} from './role.schema';
import type { Role } from './role.types';

export const getOptions = (signal?: AbortSignal) =>
    request.get<Role.Option[]>('/v1/roles/options', { signal });

export const getList = (params?: Role.Query, signal?: AbortSignal) =>
    request.get<PaginatedResult<Role.Item>>('/v1/roles', {
        params: roleListQuerySchema.parse(params ?? {}),
        signal,
    });
export const create = (data: Role.CreateDto) =>
    request.post<void>('/v1/roles', createRoleSchema.parse(data));
export const update = (id: string, data: Role.UpdateDto) =>
    request.put<void>(`/v1/roles/${roleIdSchema.parse(id)}`, updateRoleSchema.parse(data));
export const remove = (id: string) => request.delete<void>(`/v1/roles/${roleIdSchema.parse(id)}`);
