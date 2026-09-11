import { useLiveQuery } from '@/hooks/useLiveQuery';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { LiveResource } from '@/utils/live-resource';
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
import { type EdgeVpn, edgeVpnQueryKeys } from './edge-node.vpn.types';

export const useEdgeVpn = (nodeId?: string) =>
    useLiveQuery({
        queryKey: edgeVpnQueryKeys.node(nodeId),
        queryFn: () =>
            LiveResource.combine([
                getVpnNetworks(),
                getEdgeVpnPeers(nodeId as string),
                getEdgeVpnRoutes(nodeId as string),
            ] as const).map(
                ([networks, peers, routes]): EdgeVpn.Data => ({
                    networks: networks.list,
                    peers,
                    routes,
                })
            ),
        enabled: Boolean(nodeId),
    });

const vpnInvalidations = [edgeVpnQueryKeys.all, edgeQueryKeys.all];

export const useVpnNetworkCreate = () =>
    useMutationWithMessage({
        mutationFn: createVpnNetwork,
        successMessage: '默认 iot-server VPN 网络已就绪',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnPeerCreate = () =>
    useMutationWithMessage({
        mutationFn: createEdgeVpnPeer,
        successMessage: '节点 VPN 已启用，正在下发桥接网段映射',
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
        successMessage: '虚拟网段已更新，正在下发配置',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnRouteDelete = () =>
    useMutationWithMessage({
        mutationFn: deleteEdgeVpnRoute,
        successMessage: 'VPN 路由已删除',
        invalidateKeys: vpnInvalidations,
    });
