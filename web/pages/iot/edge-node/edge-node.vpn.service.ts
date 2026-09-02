import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { edgeQueryKeys } from './edge-node.types';
import {
    createEdgeVpnPeer,
    createEdgeVpnRoute,
    createVpnNetwork,
    deleteEdgeVpnRoute,
    getEdgeVpnPeers,
    getEdgeVpnRoutes,
    getVpnNetworks,
    revokeEdgeVpnPeer,
    syncEdgeVpnPeer,
    updateEdgeVpnRoute,
} from './edge-node.vpn.client';
import { edgeVpnQueryKeys, type EdgeVpn } from './edge-node.vpn.types';

export const useEdgeVpn = (nodeId?: string) =>
    useQuery({
        queryKey: edgeVpnQueryKeys.node(nodeId),
        queryFn: async (): Promise<EdgeVpn.Data> => {
            const [networks, peers, routes] = await Promise.all([
                getVpnNetworks(),
                getEdgeVpnPeers(nodeId as string),
                getEdgeVpnRoutes(nodeId as string),
            ]);
            return { networks: networks.list, peers, routes };
        },
        enabled: Boolean(nodeId),
        refetchInterval: 10_000,
    });

const vpnInvalidations = [edgeVpnQueryKeys.all, edgeQueryKeys.all];

export const useVpnNetworkCreate = () =>
    useMutationWithMessage({
        mutationFn: createVpnNetwork,
        successMessage: 'VPN 网络已创建',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnPeerCreate = () =>
    useMutationWithMessage({
        mutationFn: createEdgeVpnPeer,
        successMessage: 'VPN 已加入当前边缘节点，正在下发配置',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnPeerSync = () =>
    useMutationWithMessage({
        mutationFn: syncEdgeVpnPeer,
        successMessage: 'VPN 配置已重新下发',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnPeerRevoke = () =>
    useMutationWithMessage({
        mutationFn: revokeEdgeVpnPeer,
        successMessage: 'VPN Peer 已撤销',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnRouteCreate = () =>
    useMutationWithMessage({
        mutationFn: createEdgeVpnRoute,
        successMessage: 'VPN 路由已保存并下发',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnRouteUpdate = () =>
    useMutationWithMessage({
        mutationFn: ({ id, data }: { id: string; data: Partial<EdgeVpn.RouteDto> }) =>
            updateEdgeVpnRoute(id, data),
        successMessage: 'VPN 路由已更新并下发',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnRouteDelete = () =>
    useMutationWithMessage({
        mutationFn: deleteEdgeVpnRoute,
        successMessage: 'VPN 路由已删除',
        invalidateKeys: vpnInvalidations,
    });
