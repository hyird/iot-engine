import request from '@/lib/http';
import type { PaginatedResult } from '@/types/pagination';
import {
    createDeptSchema,
    deptIdSchema,
    deptListQuerySchema,
    updateDeptSchema,
} from './dept.schema';
import type { Dept } from './dept.types';

export const getList = (params?: Dept.Query, signal?: AbortSignal) =>
    request.get<PaginatedResult<Dept.Item>>('/v1/departments', {
        params: deptListQuerySchema.parse(params ?? {}),
        signal,
    });
export const getOptions = (signal?: AbortSignal) =>
    request.get<Dept.Option[]>('/v1/departments/options', { signal });
export const create = (data: Dept.CreateDto) =>
    request.post<void>('/v1/departments', createDeptSchema.parse(data));
export const update = (id: string, data: Dept.UpdateDto) =>
    request.put<void>(`/v1/departments/${deptIdSchema.parse(id)}`, updateDeptSchema.parse(data));
export const remove = (id: string) =>
    request.delete<void>(`/v1/departments/${deptIdSchema.parse(id)}`);
