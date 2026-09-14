import request from '@/utils/http';
import type { PaginatedResult } from '@/utils/pagination';
import { appendQueryParams } from '@/utils/query';
import { createSnapshotStream } from '@/utils/snapshot-request';
import {
    edgeGroupSchema,
    edgeIdSchema,
    edgeListQuerySchema,
    firmwareUpgradeSchema,
    logLevelSchema,
    logsQuerySchema,
    networkSchema,
    nodeGroupSchema,
    nodeNameSchema,
} from './edge_node.schema';
import type { Edge, EdgeVpn } from './edge_node.types';

const BASE = '/v1/edge';
export const getEdgeList = (query?: Edge.Query) =>
    createSnapshotStream<PaginatedResult<Edge.Node>>(
        appendQueryParams(BASE, edgeListQuerySchema.parse(query ?? {}))
    );
export const getEdgeDetail = (id: string) =>
    createSnapshotStream<Edge.Node>(`${BASE}/${edgeIdSchema.parse(id)}`);
const buildGroupTree = (items: Edge.GroupItem[]) => {
    const index = new Map<string, Edge.GroupTreeItem>();
    const roots: Edge.GroupTreeItem[] = [];
    for (const item of items) index.set(item.id, { ...item, children: [] });
    for (const item of index.values()) {
        const parent = item.parentId ? index.get(item.parentId) : undefined;
        if (parent) parent.children?.push(item);
        else roots.push(item);
    }
    return roots;
};
export const getEdgeGroups = () =>
    createSnapshotStream<Edge.GroupItem[]>(`${BASE}/groups`).map(buildGroupTree);
export const createEdgeGroup = (data: Edge.GroupSaveDto) =>
    request.post<void>(`${BASE}/groups`, edgeGroupSchema.parse(data));
export const updateEdgeGroup = (id: string, data: Edge.GroupSaveDto) =>
    request.put<void>(`${BASE}/groups/${edgeIdSchema.parse(id)}`, edgeGroupSchema.parse(data));
export const deleteEdgeGroup = (id: string) =>
    request.delete<void>(`${BASE}/groups/${edgeIdSchema.parse(id)}`);
export const getLogs = (id: string, query?: Edge.LogsQuery) =>
    createSnapshotStream<Edge.Logs>(
        appendQueryParams(
            `${BASE}/${edgeIdSchema.parse(id)}/logs`,
            logsQuerySchema.parse(query ?? {})
        )
    );
export const captureLogs = (id: string) =>
    request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/logs/capture`);
export const setLogLevel = (id: string, data: Edge.LogLevelDto) =>
    request.put<void>(`${BASE}/${edgeIdSchema.parse(id)}/logs/level`, logLevelSchema.parse(data));
export const setEnrollment = (id: string, status: 'approved', name?: string) =>
    request.put<void>(`${BASE}/${edgeIdSchema.parse(id)}/enrollment`, { status, name });
export const deleteEnrollment = (id: string) =>
    request.delete<void>(`${BASE}/${edgeIdSchema.parse(id)}`);
export const renameEdge = (id: string, data: Edge.NameDto) =>
    request.put<void>(`${BASE}/${edgeIdSchema.parse(id)}/name`, nodeNameSchema.parse(data));
export const setEdgeGroup = (id: string, data: Edge.GroupDto) =>
    request.put<void>(`${BASE}/${edgeIdSchema.parse(id)}/group`, nodeGroupSchema.parse(data));
export const configureNetwork = (id: string, data: Edge.NetworkDto) =>
    request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/network`, networkSchema.parse(data));
export const syncDeviceConfig = (id: string) =>
    request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/sync`);
export const upgradeFirmware = (
    id: string,
    data: Edge.FirmwareUpgradeDto,
    onProgress?: (progress: Edge.FirmwareUploadProgress) => void
) => {
    const value = firmwareUpgradeSchema.parse(data);
    const form = new FormData();
    form.append('keepSettings', String(value.keepSettings));
    form.append('file', value.file, value.file.name);
    return request.post<void>(`${BASE}/${edgeIdSchema.parse(id)}/firmware`, form, {
        timeout: 5 * 60 * 1000,
        onUploadProgress: onProgress
            ? (event) => {
                  const totalBytes = value.file.size;
                  const ratio = event.total
                      ? event.loaded / event.total
                      : (event.progress ?? event.loaded / totalBytes);
                  const loadedBytes = Math.min(
                      totalBytes,
                      Math.max(0, Math.round(totalBytes * ratio))
                  );
                  onProgress({
                      loadedBytes,
                      totalBytes,
                      percent: Math.min(100, Math.round((loadedBytes / totalBytes) * 100)),
                  });
              }
            : undefined,
    });
};
export const getTerminalTicket = (id: string) =>
    request.post<{
        ticket: string;
    }>(`${BASE}/${edgeIdSchema.parse(id)}/terminal-ticket`);

const EdgeNodeBASE = '/v1/vpn';
export const getVpnNetworks = () =>
    createSnapshotStream<PaginatedResult<EdgeVpn.Network>>(
        appendQueryParams(`${EdgeNodeBASE}/networks`, { page: 1, pageSize: 100, status: 'enabled' })
    );
export const getEdgeVpnPeers = (nodeId: string) =>
    createSnapshotStream<EdgeVpn.Peer[]>(
        appendQueryParams(`${EdgeNodeBASE}/peers`, { edgeNodeId: edgeIdSchema.parse(nodeId) })
    );
export const getEdgeVpnRoutes = (nodeId: string) =>
    createSnapshotStream<EdgeVpn.Route[]>(
        appendQueryParams(`${EdgeNodeBASE}/routes`, { edgeNodeId: edgeIdSchema.parse(nodeId) })
    );
export const createVpnNetwork = (data: EdgeVpn.NetworkCreateDto) =>
    request.post<{
        id: string;
    }>(`${EdgeNodeBASE}/networks`, data);
export const createEdgeVpnPeer = (data: EdgeVpn.PeerCreateDto) =>
    request.post<{
        id: string;
    }>(`${EdgeNodeBASE}/peers`, data);
export const syncEdgeVpnPeer = (peerId: string) =>
    request.post<void>(`${EdgeNodeBASE}/peers/${edgeIdSchema.parse(peerId)}/sync`);
export const revokeEdgeVpnPeer = (peerId: string) =>
    request.post<void>(`${EdgeNodeBASE}/peers/${edgeIdSchema.parse(peerId)}/revoke`);
export const createEdgeVpnRoute = (data: EdgeVpn.RouteDto) =>
    request.post<{
        id: string;
    }>(`${EdgeNodeBASE}/routes`, data);
export const updateEdgeVpnRoute = (routeId: string, data: Partial<EdgeVpn.RouteDto>) =>
    request.patch<void>(`${EdgeNodeBASE}/routes/${edgeIdSchema.parse(routeId)}`, data);
export const deleteEdgeVpnRoute = (routeId: string) =>
    request.delete<void>(`${EdgeNodeBASE}/routes/${edgeIdSchema.parse(routeId)}`);
