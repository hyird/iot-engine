import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import type { Protocol } from './protocol.types';
import {
    protocolCreateSchema,
    protocolIdSchema,
    protocolTypeSchema,
    protocolUpdateSchema,
} from './protocol.schema';

const BASE = '/api/protocol/configs';

export const getList = (params?: Protocol.Query) =>
    request.get<PaginatedResult<Protocol.Item>>(appendQueryParams(BASE, params));
export const getDetail = (id: number) =>
    request.get<Protocol.Item>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const create = (data: Protocol.CreateDto) =>
    request.post<void>(BASE, protocolCreateSchema.parse(data));
export const update = (id: number, data: Protocol.UpdateDto) =>
    request.put<void>(`${BASE}/${protocolIdSchema.parse(id)}`, protocolUpdateSchema.parse(data));
export const remove = (id: number) => request.delete<void>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const getOptions = (protocol: Protocol.Type) =>
    request.get<PaginatedResult<Protocol.Option>>(
        appendQueryParams(`${BASE}/options`, { protocol: protocolTypeSchema.parse(protocol) })
    );
