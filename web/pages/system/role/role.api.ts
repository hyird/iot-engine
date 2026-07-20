import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import type { Role } from './role.types';

const BASE = '/api/roles';

export const getList = (params?: Role.Query) =>
    request.get<PaginatedResult<Role.Item>>(appendQueryParams(BASE, params));
export const getDetail = (id: number) => request.get<Role.Item>(`${BASE}/${id}`);
export const create = (data: Role.CreateDto) => request.post<void>(BASE, data);
export const update = (id: number, data: Role.UpdateDto) =>
    request.put<void>(`${BASE}/${id}`, data);
export const remove = (id: number) => request.delete<void>(`${BASE}/${id}`);
