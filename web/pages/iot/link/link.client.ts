import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import { linkIdSchema, linkListQuerySchema, saveLinkSchema } from './link.schema';
import type { Link } from './link.types';

const BASE = '/v1/link';

export const getList = (params?: Link.Query) =>
    request.get<PaginatedResult<Link.Item>>(
        appendQueryParams(BASE, linkListQuerySchema.parse(params ?? {}))
    );
export const getEnums = () => request.get<Link.Enums>(`${BASE}/enums`);
export const getOptions = () => request.get<Link.Option[]>(`${BASE}/options`);
export async function getPublicIp() {
    try {
        return await request.get<{ ip: string }>(`${BASE}/public-ip`, { _silent: true });
    } catch {
        return { ip: '' };
    }
}
export const create = (data: Link.SaveDto) => request.post<void>(BASE, saveLinkSchema.parse(data));
export const update = (id: string, data: Link.SaveDto) =>
    request.put<void>(`${BASE}/${linkIdSchema.parse(id)}`, saveLinkSchema.parse(data));
export const remove = (id: string) => request.delete<void>(`${BASE}/${linkIdSchema.parse(id)}`);
