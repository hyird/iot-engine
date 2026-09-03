import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { edgeQueryKeys } from './edge-node.types';
import {
    createEdgeVpnPeer,
    createEdgeVpnRoute,
    createVpnNetwork,
    createWindowsVpnConfig,
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
        successMessage: '默认 iot-server VPN 网络已就绪',
        invalidateKeys: vpnInvalidations,
    });

export const useEdgeVpnPeerCreate = () =>
    useMutationWithMessage({
        mutationFn: createEdgeVpnPeer,
        successMessage: '节点 VPN 已启用，正在下发桥接网段映射',
        invalidateKeys: vpnInvalidations,
    });

export const useWindowsVpnConfigCreate = () =>
    useMutationWithMessage({
        mutationFn: createWindowsVpnConfig,
        successMessage: 'Windows WireGuard 配置已生成并下载',
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
