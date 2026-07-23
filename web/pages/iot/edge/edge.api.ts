import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import {
    edgeIdSchema,
    edgeListQuerySchema,
    firmwareUpgradeSchema,
    networkSchema,
    nodeNameSchema,
    platformIdSchema,
    platformSchema,
} from './edge.schema';
import type { Edge } from './edge.types';

const BASE = '/api/edge';

export const getEdgeList = (query?: Edge.Query) =>
    request.get<PaginatedResult<Edge.Node>>(
        appendQueryParams(BASE, edgeListQuerySchema.parse(query ?? {}))
    );
export const getEdgeDetail = (id: string) =>
    request.get<Edge.Node>(`${BASE}/${edgeIdSchema.parse(id)}`);
export const setEnrollment = (id: string, status: 'approved' | 'rejected', name?: string) =>
    request.put<void>(`${BASE}/${edgeIdSchema.parse(id)}/enrollment`, { status, name });
export const renameEdge = (id: string, data: Edge.NameDto) =>
    request.put<void>(`${BASE}/${edgeIdSchema.parse(id)}/name`, nodeNameSchema.parse(data));
export const configureNetwork = (id: string, data: Edge.NetworkDto) =>
    request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/network`, networkSchema.parse(data));
export const syncDeviceConfig = (id: string) =>
    request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/sync`);
export const configurePlatform = (id: string, data: Edge.PlatformDto) =>
    request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/platforms`, platformSchema.parse(data));
export const removePlatform = (id: string, platformId: string) =>
    request.delete<void>(
        `${BASE}/${edgeIdSchema.parse(id)}/platforms/${platformIdSchema.parse(platformId)}`
    );
export const upgradeFirmware = (id: string, data: Edge.FirmwareUpgradeDto) => {
    const value = firmwareUpgradeSchema.parse(data);
    const form = new FormData();
    form.append('version', value.version);
    form.append('keepSettings', String(value.keepSettings));
    form.append('file', value.file, value.file.name);
    return request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/firmware`, form, {
        timeout: 5 * 60 * 1000,
    });
};
export const getTerminalTicket = (id: string) =>
    request.post<{ ticket: string }>(`${BASE}/${edgeIdSchema.parse(id)}/terminal-ticket`);
