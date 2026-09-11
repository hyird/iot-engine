import { createSnapshotStream } from '@/utils/snapshot-request';
import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/pagination';
import { linkIdSchema, linkListQuerySchema, saveLinkSchema } from './link.schema';
import type { Link } from './link.types';

const BASE = '/v1/link';

export const getList = (params?: Link.Query) =>
    createSnapshotStream<PaginatedResult<Link.Item>>(
        appendQueryParams(BASE, linkListQuerySchema.parse(params ?? {}))
    );
export const getEnums = () => createSnapshotStream<Link.Enums>(`${BASE}/enums`);
export const getOptions = () => createSnapshotStream<Link.Option[]>(`${BASE}/options`);
export const getPublicIp = () => createSnapshotStream<{ ip: string }>(`${BASE}/public-ip`);
export const create = (data: Link.SaveDto) => request.post<void>(BASE, saveLinkSchema.parse(data));
export const update = (id: string, data: Link.SaveDto) =>
    request.put<void>(`${BASE}/${linkIdSchema.parse(id)}`, saveLinkSchema.parse(data));
export const remove = (id: string) => request.delete<void>(`${BASE}/${linkIdSchema.parse(id)}`);
