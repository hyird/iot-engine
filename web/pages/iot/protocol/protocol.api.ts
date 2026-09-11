import { SnapshotStream } from '@/utils/snapshot-stream';
import { createSnapshotStream } from '@/utils/snapshot-request';
import request, { type RequestConfig } from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/pagination';
import {
    protocolCreateSchema,
    protocolIdSchema,
    protocolTypeSchema,
    protocolUpdateSchema,
} from './protocol.schema';
import type { Protocol } from './protocol.types';

const BASE = '/v1/protocol/configs';
const MAX_PAGE_SIZE = 1000;

export const getList = (params?: Protocol.Query, config?: RequestConfig) =>
    createSnapshotStream<PaginatedResult<Protocol.Item>>(appendQueryParams(BASE, params), config);
export const getAll = (params?: Protocol.Query, requestConfig?: RequestConfig) => {
    const { page: _page, pageSize: _pageSize, ...filters } = params ?? {};
    return getList({ ...filters, page: 1, pageSize: MAX_PAGE_SIZE }, requestConfig).switchMap(
        (first) => {
            const count = Math.max(1, first.totalPages ?? Math.ceil(first.total / MAX_PAGE_SIZE));
            const pages = Array.from({ length: count }, (_, index) =>
                index === 0
                    ? SnapshotStream.value(first)
                    : getList(
                          { ...filters, page: index + 1, pageSize: MAX_PAGE_SIZE },
                          requestConfig
                      )
            );
            return SnapshotStream.combine(pages).map((results) =>
                results.flatMap((page) => page.list)
            );
        }
    );
};
export const getDetail = (id: string) =>
    createSnapshotStream<Protocol.Item>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const create = (data: Protocol.CreateDto, config?: RequestConfig) =>
    request.post<void>(BASE, protocolCreateSchema.parse(data), config);
export const update = (id: string, data: Protocol.UpdateDto) =>
    request.post<void>(
        `${BASE}/${protocolIdSchema.parse(id)}/revisions`,
        protocolUpdateSchema.parse(data)
    );
export const getRevisions = (id: string) =>
    createSnapshotStream<{ revision: number; name: string; origin: string; created_at: string }[]>(
        `${BASE}/${protocolIdSchema.parse(id)}/revisions`
    );
export const remove = (id: string) => request.delete<void>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const getOptions = (protocol: Protocol.Type) =>
    createSnapshotStream<PaginatedResult<Protocol.Option>>(
        appendQueryParams(`${BASE}/options`, { protocol: protocolTypeSchema.parse(protocol) })
    );
