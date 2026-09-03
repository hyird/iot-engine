import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import { edgeIdSchema } from './edge-node.schema';
import type { EdgeVpn } from './edge-node.vpn.types';

const BASE = '/v1/vpn';

export const getVpnNetworks = () =>
    request.get<PaginatedResult<EdgeVpn.Network>>(
        appendQueryParams(`${BASE}/networks`, { page: 1, pageSize: 100, status: 'enabled' })
    );

export const getEdgeVpnPeers = (nodeId: string) =>
    request.get<EdgeVpn.Peer[]>(
        appendQueryParams(`${BASE}/peers`, { edgeNodeId: edgeIdSchema.parse(nodeId) })
    );

export const getEdgeVpnRoutes = (nodeId: string) =>
    request.get<EdgeVpn.Route[]>(
        appendQueryParams(`${BASE}/routes`, { edgeNodeId: edgeIdSchema.parse(nodeId) })
    );

export const createVpnNetwork = (data: EdgeVpn.NetworkCreateDto) =>
    request.post<{ id: string }>(`${BASE}/networks`, data);

export const createEdgeVpnPeer = (data: EdgeVpn.PeerCreateDto) =>
    request.post<{ id: string }>(`${BASE}/peers`, data);

export const createWindowsVpnConfig = (data: EdgeVpn.ClientConfigCreateDto) =>
    request.post<EdgeVpn.ClientConfig>(`${BASE}/client-configs`, data);

export const syncEdgeVpnPeer = (peerId: string) =>
    request.post<void>(`${BASE}/peers/${edgeIdSchema.parse(peerId)}/sync`);

export const revokeEdgeVpnPeer = (peerId: string) =>
    request.post<void>(`${BASE}/peers/${edgeIdSchema.parse(peerId)}/revoke`);

export const createEdgeVpnRoute = (data: EdgeVpn.RouteDto) =>
    request.post<{ id: string }>(`${BASE}/routes`, data);

export const updateEdgeVpnRoute = (routeId: string, data: Partial<EdgeVpn.RouteDto>) =>
    request.patch<void>(`${BASE}/routes/${edgeIdSchema.parse(routeId)}`, data);

export const deleteEdgeVpnRoute = (routeId: string) =>
    request.delete<void>(`${BASE}/routes/${edgeIdSchema.parse(routeId)}`);
