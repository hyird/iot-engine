import { LiveResource } from '@/utils/live-resource';
import { useEffect } from 'react';
import { useLiveQuery } from '@/hooks/useLiveQuery';
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
    captureLogs,
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
    useLiveQuery({
        queryKey: edgeQueryKeys.list(query),
        queryFn: () => getEdgeList(query),
        enabled,
    });

// Match device management: group complete inventories, never just one page.
export const useEdgeInventory = (enabled = true) =>
    useLiveQuery({
        queryKey: [...edgeQueryKeys.all, 'inventory'],
        queryFn: () => getEdgeList({ page: 1, pageSize: 100 }).switchMap((first) => {
            const pages = Array.from({ length: Math.max(1, Math.ceil(first.total / 100)) }, (_, index) =>
                index === 0 ? LiveResource.value(first) : getEdgeList({ page: index + 1, pageSize: 100 }));
            return LiveResource.combine(pages).map((results) =>
                [...new Map(results.flatMap((result) => result.list).map((node) => [node.id, node])).values()]);
        }),
        enabled,
        refetchOnWindowFocus: false,
    });

export const useEdgeDetail = (id?: string) =>
    useLiveQuery({
        queryKey: edgeQueryKeys.detail(id),
        queryFn: () => getEdgeDetail(id as string),
        enabled: Boolean(id),
    });

export const useEdgeGroupTree = () =>
    useLiveQuery({
        queryKey: edgeQueryKeys.groups(),
        queryFn: getEdgeGroups,
    });

export const useEdgeLogs = (id?: string, query?: Edge.LogsQuery, enabled = true) => {
    const result = useLiveQuery({
        queryKey: edgeQueryKeys.logs(id, query),
        queryFn: () => getLogs(id as string, query),
        enabled: enabled && Boolean(id),
        staleTime: 0,
    });
    useEffect(() => {
        if (enabled && id) void captureLogs(id).catch(() => undefined);
    }, [enabled, id]);
    return { ...result, refetch: () => id ? captureLogs(id) : Promise.resolve() };
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
