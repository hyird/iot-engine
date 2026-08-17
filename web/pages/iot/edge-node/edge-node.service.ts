import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import {
    configureNetwork,
    controlModem,
    getEdgeDetail,
    getEdgeList,
    getLogs,
    renameEdge,
    setEnrollment,
    setLogLevel,
    syncDeviceConfig,
    upgradeFirmware,
} from './edge-node.client';
import { type Edge, edgeQueryKeys } from './edge-node.types';

export const useEdgeList = (query?: Edge.Query, enabled = true) =>
    useQuery({
        queryKey: edgeQueryKeys.list(query),
        queryFn: () => getEdgeList(query),
        enabled,
        refetchInterval: 2_000,
    });

export const useEdgeDetail = (id?: string) =>
    useQuery({
        queryKey: edgeQueryKeys.detail(id),
        queryFn: () => getEdgeDetail(id as string),
        enabled: Boolean(id),
        refetchInterval: 10_000,
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
        mutationFn: (value: { id: string; status: 'approved' | 'rejected'; name?: string }) =>
            setEnrollment(value.id, value.status, value.name),
        successMessage: '注册状态已更新',
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

export function useNetworkMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.NetworkDto }) =>
            configureNetwork(value.id, value.data),
        successMessage: '网络配置已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function useModemControlMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.ModemControlDto }) =>
            controlModem(value.id, value.data),
        successMessage: '移动网络操作已下发',
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
