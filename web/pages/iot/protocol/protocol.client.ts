import request, { type RequestConfig } from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import {
    protocolCreateSchema,
    protocolIdSchema,
    protocolTypeSchema,
    protocolUpdateSchema,
} from './protocol.schema';
import type { Protocol } from './protocol.types';

const BASE = '/api/protocol/configs';
const MAX_PAGE_SIZE = 1000;

export const getList = (params?: Protocol.Query, config?: RequestConfig) =>
    request.get<PaginatedResult<Protocol.Item>>(appendQueryParams(BASE, params), config);
export const getAll = async (params?: Protocol.Query, requestConfig?: RequestConfig) => {
    const { page: _page, pageSize: _pageSize, ...filters } = params ?? {};
    const firstPage = await getList(
        { ...filters, page: 1, pageSize: MAX_PAGE_SIZE },
        requestConfig
    );
    const items = [...firstPage.list];
    const totalPages =
        firstPage.totalPages ?? Math.ceil(firstPage.total / (firstPage.pageSize ?? MAX_PAGE_SIZE));

    for (let page = 2; page <= totalPages; page++) {
        const nextPage = await getList(
            { ...filters, page, pageSize: MAX_PAGE_SIZE },
            requestConfig
        );
        items.push(...nextPage.list);
    }

    return items;
};
export const getDetail = (id: string) =>
    request.get<Protocol.Item>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const create = (data: Protocol.CreateDto, config?: RequestConfig) =>
    request.post<void>(BASE, protocolCreateSchema.parse(data), config);
export const update = (id: string, data: Protocol.UpdateDto) =>
    request.put<void>(`${BASE}/${protocolIdSchema.parse(id)}`, protocolUpdateSchema.parse(data));
export const remove = (id: string) => request.delete<void>(`${BASE}/${protocolIdSchema.parse(id)}`);
export const getOptions = (protocol: Protocol.Type) =>
    request.get<PaginatedResult<Protocol.Option>>(
        appendQueryParams(`${BASE}/options`, { protocol: protocolTypeSchema.parse(protocol) })
    );
