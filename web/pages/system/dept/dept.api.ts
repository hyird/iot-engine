import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import type { Dept } from './dept.types';

const BASE = '/api/departments';
export const getList = (params?: Dept.Query) =>
    request.get<PaginatedResult<Dept.Item>>(appendQueryParams(BASE, params));
export const getOptions = () => request.get<Dept.Option[]>(`${BASE}/options`);
export const create = (data: Dept.CreateDto) => request.post<void>(BASE, data);
export const update = (id: number, data: Dept.UpdateDto) =>
    request.put<void>(`${BASE}/${id}`, data);
export const remove = (id: number) => request.delete<void>(`${BASE}/${id}`);
