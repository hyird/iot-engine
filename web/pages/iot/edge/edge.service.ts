import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import {
    configureNetwork,
    configurePlatform,
    getEdgeDetail,
    getEdgeList,
    removePlatform,
    renameEdge,
    setEnrollment,
    syncDeviceConfig,
    upgradeFirmware,
} from './edge.api';
import { type Edge, edgeQueryKeys } from './edge.types';

export const useEdgeList = (query?: Edge.Query, enabled = true) =>
    useQuery({ queryKey: edgeQueryKeys.list(query), queryFn: () => getEdgeList(query), enabled });

export const useEdgeDetail = (id?: string) =>
    useQuery({
        queryKey: edgeQueryKeys.detail(id),
        queryFn: () => getEdgeDetail(id as string),
        enabled: Boolean(id),
        refetchInterval: 10_000,
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

export function useDeviceConfigSyncMutation() {
    return useMutationWithMessage({
        mutationFn: syncDeviceConfig,
        successMessage: '设备配置已生成并下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function usePlatformMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.PlatformDto }) =>
            configurePlatform(value.id, value.data),
        successMessage: '平台配置已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function usePlatformDeleteMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; platformId: string }) =>
            removePlatform(value.id, value.platformId),
        successMessage: '平台删除配置已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function useFirmwareUpgradeMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.FirmwareUpgradeDto }) =>
            upgradeFirmware(value.id, value.data),
        successMessage: '固件已上传，刷写任务已下发给当前节点',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
