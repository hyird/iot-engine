import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import { linkIdSchema, linkListQuerySchema, saveLinkSchema } from './link.schema';
import type { Link } from './link.types';

const BASE = '/api/link';

export const getList = (params?: Link.Query) =>
    request.get<PaginatedResult<Link.Item>>(
        appendQueryParams(BASE, linkListQuerySchema.parse(params ?? {}))
    );
export const create = (data: Link.SaveDto) => request.post<void>(BASE, saveLinkSchema.parse(data));
export const update = (id: number, data: Link.SaveDto) =>
    request.put<void>(`${BASE}/${linkIdSchema.parse(id)}`, saveLinkSchema.parse(data));
export const remove = (id: number) => request.delete<void>(`${BASE}/${linkIdSchema.parse(id)}`);
