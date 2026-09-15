import type { RequestConfig } from '@/lib/http';
import request from '@/lib/http';
import type { PaginatedResult } from '@/types/pagination';
import { appendQueryParams } from '@/utils/query';
import { createSnapshotStream } from '@/lib/snapshot-request';
import {
    protocolCreateSchema,
    protocolIdSchema,
    protocolTypeSchema,
    protocolUpdateSchema,
} from './protocol.schema';
import type { Protocol } from './protocol.types';

const BASE = '/v1/protocol/configs';
export const getList = (params?: Protocol.Query, config?: RequestConfig) =>
    createSnapshotStream<PaginatedResult<Protocol.Item>>(appendQueryParams(BASE, params), config);
export const getDetail = (id: string) =>
    createSnapshotStream<Protocol.Item>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const create = (data: Protocol.CreateDto, config?: RequestConfig) =>
    request.post<void>(BASE, protocolCreateSchema.parse(data), config);
export const update = (id: string, data: Protocol.UpdateDto) =>
    request.put<void>(`${BASE}/${protocolIdSchema.parse(id)}`, protocolUpdateSchema.parse(data));
export const remove = (id: string) => request.delete<void>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const getOptions = (protocol: Protocol.Type) =>
    createSnapshotStream<PaginatedResult<Protocol.Option>>(
        appendQueryParams(`${BASE}/options`, { protocol: protocolTypeSchema.parse(protocol) })
    );
