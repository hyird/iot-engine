import { useEffect } from 'react';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import { SnapshotStream } from '@/lib/snapshot-stream';
import {
    captureLogs,
    configureNetwork,
    createEdgeGroup,
    createEdgeVpnPeer,
    createEdgeVpnRoute,
    createVpnNetwork,
    deleteEdgeGroup,
    deleteEdgeVpnRoute,
    deleteEnrollment,
    getEdgeDetail,
    getEdgeGroups,
    getEdgeList,
    getEdgeVpnPeers,
    getEdgeVpnRoutes,
    getLogs,
    getVpnNetworks,
    renameEdge,
    revokeEdgeVpnPeer,
    setEdgeGroup,
    setEnrollment,
    setLogLevel,
    syncDeviceConfig,
    syncEdgeVpnPeer,
    updateEdgeGroup,
    updateEdgeVpnRoute,
    upgradeFirmware,
} from './edge_node.api';
import type { Edge, EdgeVpn } from './edge_node.types';
import { edgeQueryKeys, edgeVpnQueryKeys } from './edge_node.types';
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
export function buildEdgeNodeGroupView(
    groups: Edge.GroupTreeItem[],
    nodes: Edge.Node[],
    selectedId: string | null,
    keyword: string,
    status?: Edge.EnrollmentStatus
) {
    const index = new Map<string, Edge.GroupTreeItem>();
    const visit = (items: Edge.GroupTreeItem[]) =>
        items.forEach((group) => {
            index.set(group.id, group);
            visit(group.children ?? []);
        });
    visit(groups);
    const selected = selectedId ? index.get(selectedId) : undefined;
    const scope = new Set<string>();
    const collect = (group: Edge.GroupTreeItem) => {
        scope.add(group.id);
        group.children?.forEach(collect);
    };
    if (selected) collect(selected);
    const query = keyword.trim().toLowerCase();
    const filtered = nodes.filter((node) => {
        if (selectedId === 'ungrouped' && node.groupId) return false;
        if (selectedId && selectedId !== 'ungrouped' && !scope.has(node.groupId ?? ''))
            return false;
        if (status && node.enrollmentStatus !== status) return false;
        return (
            !query ||
            [node.name, node.imei, node.model, node.hostname].some((value) =>
                value?.toLowerCase().includes(query)
            )
        );
    });
    const direct = new Map<string, Edge.Node[]>();
    for (const node of filtered) {
        const id = node.groupId ?? '';
        const items = direct.get(id) ?? [];
        items.push(node);
        direct.set(id, items);
    }
    const stats = new Map<
        string,
        {
            total: number;
            online: number;
            offline: number;
        }
    >();
    const count = (group: Edge.GroupTreeItem) => {
        const own = direct.get(group.id) ?? [];
        const value = {
            total: own.length,
            online: own.filter((node) => node.status.online).length,
            offline: 0,
        };
        for (const child of group.children ?? []) {
            const childStats = count(child);
            value.total += childStats.total;
            value.online += childStats.online;
        }
        value.offline = value.total - value.online;
        stats.set(group.id, value);
        return value;
    };
    groups.forEach(count);
    return {
        filtered,
        direct,
        stats,
        roots: selectedId === 'ungrouped' ? [] : selectedId ? (selected ? [selected] : []) : groups,
        ungrouped: filtered.filter((node) => !node.groupId),
        ungroupedCount: nodes.filter((node) => !node.groupId).length,
    };
}

const platformVirtualInterfaces = new Set(['lo', 'wg']);
export function physicalNetworkInterfaces(interfaces: Edge.NetworkInterface[]) {
    const subinterfaceParents = new Set<string>();
    for (const item of interfaces) {
        let parent = item.name;
        while (parent.includes('.')) {
            parent = parent.slice(0, parent.lastIndexOf('.'));
            if (parent) subinterfaceParents.add(parent);
        }
    }
    return interfaces.filter(
        (item) =>
            !item.bridge &&
            !subinterfaceParents.has(item.name) &&
            !platformVirtualInterfaces.has(item.name)
    );
}
export function normalizeReportedNetwork(
    network: Edge.Network,
    interfaces: Edge.NetworkInterface[]
) {
    const reportedBridge = interfaces.find(
        (candidate) => candidate.name === network.device && candidate.bridge
    );
    const bridge = network.bridge || Boolean(reportedBridge);
    return {
        bridge,
        bridgePorts: bridge
            ? network.bridgePorts.length > 0
                ? network.bridgePorts
                : (reportedBridge?.bridgePorts ?? [])
            : [],
        device: bridge ? '' : network.device,
    };
}

export const useEdgeList = (query?: Edge.Query, enabled = true) =>
    useSnapshotQuery({
        queryKey: edgeQueryKeys.list(query),
        queryFn: () => getEdgeList(query),
        enabled,
    });
// Match device management: group complete inventories, never just one page.
export const useEdgeInventory = (enabled = true) =>
    useSnapshotQuery({
        queryKey: [...edgeQueryKeys.all, 'inventory'],
        queryFn: () =>
            getEdgeList({ page: 1, pageSize: 100 }).switchMap((first) => {
                const pages = Array.from(
                    { length: Math.max(1, Math.ceil(first.total / 100)) },
                    (_, index) =>
                        index === 0
                            ? SnapshotStream.value(first)
                            : getEdgeList({ page: index + 1, pageSize: 100 })
                );
                return SnapshotStream.combine(pages).map((results) => [
                    ...new Map(
                        results.flatMap((result) => result.list).map((node) => [node.id, node])
                    ).values(),
                ]);
            }),
        enabled,
        refetchOnWindowFocus: false,
    });
export const useEdgeDetail = (id?: string) =>
    useSnapshotQuery({
        queryKey: edgeQueryKeys.detail(id),
        queryFn: () => getEdgeDetail(id as string),
        enabled: Boolean(id),
    });
export const useEdgeGroupTree = () =>
    useSnapshotQuery({
        queryKey: edgeQueryKeys.groups(),
        queryFn: () => getEdgeGroups().map(buildGroupTree),
    });
export const useEdgeLogs = (id?: string, query?: Edge.LogsQuery, enabled = true) => {
    const result = useSnapshotQuery({
        queryKey: edgeQueryKeys.logs(id, query),
        queryFn: () => getLogs(id as string, query),
        enabled: enabled && Boolean(id),
        staleTime: 0,
    });
    useEffect(() => {
        if (enabled && id) void captureLogs(id).catch(() => undefined);
    }, [enabled, id]);
    return { ...result, refetch: () => (id ? captureLogs(id) : Promise.resolve()) };
};
export function useEnrollmentMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; status: 'approved'; name?: string }) =>
            setEnrollment(value.id, value.status, value.name),
        successMessage: '注册状态已更新',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useEdgeDeleteMutation() {
    return useMutationWithMessage({
        mutationFn: deleteEnrollment,
        successMessage: '注册申请已删除，节点需要重新注册',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useRenameEdgeNode() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.NameDto }) => renameEdge(value.id, value.data),
        successMessage: '节点名称已更新',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useAssignEdgeNodeGroup() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.GroupDto }) =>
            setEdgeGroup(value.id, value.data),
        successMessage: '节点分组已更新',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useEdgeGroupSave() {
    return useSaveMutation<
        Edge.GroupSaveDto & {
            id?: string;
        },
        Edge.GroupSaveDto,
        Edge.GroupSaveDto
    >({
        createFn: createEdgeGroup,
        updateFn: updateEdgeGroup,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '边缘节点分组已创建',
        updateMessage: '边缘节点分组已更新',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useEdgeGroupDelete() {
    return useMutationWithMessage({
        mutationFn: deleteEdgeGroup,
        successMessage: '边缘节点分组已删除',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useConfigureEdgeNetwork() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.NetworkDto }) =>
            configureNetwork(value.id, value.data),
        successMessage: '网络配置已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useDeviceConfigSyncMutation() {
    return useMutationWithMessage({
        mutationFn: syncDeviceConfig,
        successMessage: '设备配置已生成并下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useFirmwareUpgradeMutation() {
    return useMutationWithMessage({
        mutationFn: (value: {
            id: string;
            data: Edge.FirmwareUpgradeDto;
            onProgress?: (progress: Edge.FirmwareUploadProgress) => void;
        }) => upgradeFirmware(value.id, value.data, value.onProgress),
        successMessage: '固件已上传，刷写任务已下发给当前节点',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
export function useSetEdgeLogLevel() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.LogLevelDto }) =>
            setLogLevel(value.id, value.data),
        successMessage: '日志等级已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export const useEdgeVpn = (nodeId?: string) =>
    useSnapshotQuery({
        queryKey: edgeVpnQueryKeys.node(nodeId),
        queryFn: () =>
            SnapshotStream.combine([
                getVpnNetworks(),
                getEdgeVpnPeers(nodeId as string),
                getEdgeVpnRoutes(nodeId as string),
            ] as const).map(
                ([networks, peers, routes]): EdgeVpn.Overview => ({
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

export { getEdgeDetail, getTerminalTicket, openTerminalSocket } from './edge_node.api';
