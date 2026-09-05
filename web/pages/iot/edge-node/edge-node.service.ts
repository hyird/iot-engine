import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import {
    configureNetwork,
    createEdgeGroup,
    deleteEdgeGroup,
    deleteEnrollment,
    getEdgeDetail,
    getEdgeList,
    getEdgeGroups,
    getLogs,
    renameEdge,
    setEdgeGroup,
    setEnrollment,
    setLogLevel,
    syncDeviceConfig,
    upgradeFirmware,
    updateEdgeGroup,
} from './edge-node.client';
import { type Edge, edgeQueryKeys } from './edge-node.types';

export const useEdgeList = (query?: Edge.Query, enabled = true) =>
    useQuery({
        queryKey: edgeQueryKeys.list(query),
        queryFn: () => getEdgeList(query),
        enabled,
        refetchInterval: 2_000,
    });

// Match device management: group complete inventories, never just one page.
export const useEdgeInventory = (enabled = true) =>
    useQuery({
        queryKey: [...edgeQueryKeys.all, 'inventory'],
        queryFn: async () => {
            const first = await getEdgeList({ page: 1, pageSize: 100 });
            const nodes = new Map(first.list.map((node) => [node.id, node]));
            const pages = Math.ceil(first.total / 100);
            for (let page = 2; page <= pages; page++) {
                const result = await getEdgeList({ page, pageSize: 100 });
                for (const node of result.list) nodes.set(node.id, node);
            }
            return [...nodes.values()];
        },
        enabled,
        refetchInterval: 5_000,
        refetchOnWindowFocus: false,
    });

export const useEdgeDetail = (id?: string) =>
    useQuery({
        queryKey: edgeQueryKeys.detail(id),
        queryFn: () => getEdgeDetail(id as string),
        enabled: Boolean(id),
        refetchInterval: 10_000,
    });

export const useEdgeGroupTree = () =>
    useQuery({
        queryKey: edgeQueryKeys.groups(),
        queryFn: getEdgeGroups,
        refetchInterval: 5_000,
    });

export const useEdgeLogs = (id?: string, query?: Edge.LogsQuery, enabled = true) =>
    useQuery({
        queryKey: edgeQueryKeys.logs(id, query),
        queryFn: () => getLogs(id as string, query),
        enabled: enabled && Boolean(id),
        staleTime: 0,
    });

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

export function useNodeNameMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.NameDto }) => renameEdge(value.id, value.data),
        successMessage: '节点名称已更新',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function useNodeGroupMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.GroupDto }) =>
            setEdgeGroup(value.id, value.data),
        successMessage: '节点分组已更新',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function useEdgeGroupSave() {
    return useSaveMutation<
        Edge.GroupSaveDto & { id?: string },
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

export function useNetworkMutation() {
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

export function useLogLevelMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.LogLevelDto }) =>
            setLogLevel(value.id, value.data),
        successMessage: '日志等级已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
