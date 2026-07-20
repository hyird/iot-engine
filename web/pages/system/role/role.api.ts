import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import type { Role } from './role.types';
import {
    createRoleSchema,
    roleIdSchema,
    roleListQuerySchema,
    updateRoleSchema,
} from './role.schema';

const BASE = '/api/roles';

export const getList = (params?: Role.Query) =>
    request.get<PaginatedResult<Role.Item>>(
        appendQueryParams(BASE, roleListQuerySchema.parse(params ?? {}))
    );
export const create = (data: Role.CreateDto) =>
    request.post<void>(BASE, createRoleSchema.parse(data));
export const update = (id: number, data: Role.UpdateDto) =>
    request.put<void>(`${BASE}/${roleIdSchema.parse(id)}`, updateRoleSchema.parse(data));
export const remove = (id: number) => request.delete<void>(`${BASE}/${roleIdSchema.parse(id)}`);
