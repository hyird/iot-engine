import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import type { Dept } from './dept.types';
import {
    createDeptSchema,
    deptIdSchema,
    deptListQuerySchema,
    updateDeptSchema,
} from './dept.schema';

const BASE = '/api/departments';
export const getList = (params?: Dept.Query) =>
    request.get<PaginatedResult<Dept.Item>>(
        appendQueryParams(BASE, deptListQuerySchema.parse(params ?? {}))
    );
export const getOptions = () => request.get<Dept.Option[]>(`${BASE}/options`);
export const create = (data: Dept.CreateDto) =>
    request.post<void>(BASE, createDeptSchema.parse(data));
export const update = (id: number, data: Dept.UpdateDto) =>
    request.put<void>(`${BASE}/${deptIdSchema.parse(id)}`, updateDeptSchema.parse(data));
export const remove = (id: number) => request.delete<void>(`${BASE}/${deptIdSchema.parse(id)}`);
