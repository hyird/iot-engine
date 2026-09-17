import { retainNewerRecords } from '@/utils/versioned-records';
import { keepPreviousData, useQuery, type UseQueryOptions } from '@tanstack/react-query';
import { setDebug as saveDebugSwitch, getDebugPackets } from './device.api';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import { parseDateTime } from '@/utils/dateTime';
import { createQueryKeys } from '@/utils/query';
import * as api from './device.api';
import type { Device, DeviceGroup } from './device.types';
export const isDeviceOnline = (device: Device.Overview, now = Date.now()) => {
    if (!device.reportTime) return false;
    const reportTime = parseDateTime(device.reportTime);
    if (!reportTime || Number.isNaN(reportTime.getTime())) return false;
    return now - reportTime.getTime() <= (device.online_timeout || 300) * 1000;
};

const deviceKeys = createQueryKeys('devices');
const deviceRealtimeSnapshotKey = [...deviceKeys.all, 'realtime'] as const;
const groupKeys = createQueryKeys('device-groups');
const shareKeys = {
    all: ['device-shares'] as const,
    list: (kind: 'device' | 'group', id: string) => ['device-shares', kind, id, 'list'] as const,
    targets: (kind: 'device' | 'group', id: string) =>
        ['device-shares', kind, id, 'targets'] as const,
};

const buildDeviceGroupTree = (items: DeviceGroup.TreeItem[]) => {
    const map = new Map<string, DeviceGroup.TreeItem>();
    const roots: DeviceGroup.TreeItem[] = [];
    for (const item of items) map.set(item.id, { ...item, children: [] });
    for (const item of map.values()) {
        const parent = item.parent_id ? map.get(item.parent_id) : undefined;
        if (parent) parent.children?.push(item);
        else roots.push(item);
    }
    return roots;
};
export function useDeviceList(options?: {
    enabled?: boolean;
    debugDeviceId?: string;
    commandIds?: string[];
}) {
    return useSnapshotQuery({
        queryKey: [...deviceKeys.lists(), options?.debugDeviceId, options?.commandIds],
        queryFn: () => api.getDeviceList(options?.debugDeviceId, options?.commandIds),
        placeholderData: keepPreviousData,
        enabled: options?.enabled ?? true,
        refetchOnWindowFocus: false,
        staleTime: 5000,
    });
}
export function useDeviceConfigurationList(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: [...deviceKeys.all, 'configuration-list'],
        queryFn: ({ signal }) => api.queryDeviceList(signal),
        enabled: options?.enabled ?? true,
        staleTime: 0,
        refetchInterval: false,
        refetchOnWindowFocus: false,
        refetchOnReconnect: false,
    });
}
export function useDeviceRealtimeSnapshot(options?: {
    enabled?: boolean;
    debugDeviceId?: string;
    commandIds?: string[];
}) {
    return useSnapshotQuery({
        queryKey: [...deviceRealtimeSnapshotKey, options?.debugDeviceId, options?.commandIds],
        queryFn: () => api.getDeviceRealtimeSnapshot(options?.debugDeviceId, options?.commandIds),
        placeholderData: keepPreviousData,
        enabled: options?.enabled ?? true,
        refetchOnWindowFocus: false,
        staleTime: 1000,
    });
}
export function useDeviceHistory(
    deviceId: string | undefined,
    query: Device.HistoryRecordQuery,
    enabled = true
) {
    return useQuery({
        queryKey: [...deviceKeys.all, 'history', deviceId ?? '', query],
        queryFn: ({ signal }) => api.getDeviceHistory(deviceId as string, query, signal),
        enabled: Boolean(deviceId) && enabled,
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useDeviceSave() {
    return useSaveMutation<
        Device.CreateDto & {
            id?: string;
        },
        Device.CreateDto,
        Device.UpdateDto
    >({
        createFn: api.createDevice,
        updateFn: api.updateDevice,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [deviceKeys.all, groupKeys.all],
    });
}
export function useDeviceDelete() {
    return useMutationWithMessage({
        mutationFn: api.removeDevice,
        successMessage: '删除成功',
        invalidateKeys: [deviceKeys.all, groupKeys.all],
    });
}
export function useDeviceCommand() {
    return useMutationWithMessage({
        mutationFn: ({ deviceId, data }: { deviceId: string; data: Device.Command }) =>
            api.createDeviceCommand(deviceId, data),
        successMessage: '指令已受理，正在等待设备执行结果',
        errorMessage: (error) => error.message,
    });
}
export function useDeviceCommandResults(
    commandIds: string[],
    enabled: boolean,
    debugDeviceId?: string
) {
    return useSnapshotQuery({
        queryKey: [...deviceKeys.all, 'command-results', commandIds, debugDeviceId],
        queryFn: () => api.getDeviceCommandStatuses(commandIds, debugDeviceId),
        enabled: enabled && commandIds.length > 0,
    });
}
export function summarizeDeviceCommandResult(result: Device.CommandStatusesResult) {
    const failed = result.statuses.find((state) =>
        ['FAILED', 'REJECTED', 'UNKNOWN', 'READBACK_MISMATCH'].includes(state.status)
    );
    if (failed)
        return {
            failed: true,
            message:
                failed.status === 'UNKNOWN'
                    ? '指令结果未知，请核对设备状态，勿直接重发'
                    : failed.reason || '设备执行指令失败',
        };
    const actualValues = result.statuses.flatMap((status) => status.actual_values ?? []);
    return {
        failed: false,
        message: actualValues.length
            ? `设备执行成功，回读：${actualValues.map((actual) => `${actual.name || actual.element_id}=${actual.value}${actual.unit || ''}`).join('，')}`
            : '设备执行成功，已收到应答',
    };
}
export function useDeviceShares(
    deviceId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useQuery({
        queryKey: shareKeys.list('device', deviceId ?? ''),
        queryFn: ({ signal }) => api.getDeviceShares(deviceId as string, signal),
        enabled: !!deviceId && (options?.enabled ?? true),
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useDeviceShareTargets(
    deviceId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useQuery({
        queryKey: shareKeys.targets('device', deviceId ?? ''),
        queryFn: ({ signal }) => api.getDeviceShareTargets(deviceId as string, signal),
        enabled: !!deviceId && (options?.enabled ?? true),
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useReplaceDeviceShares() {
    return useMutationWithMessage({
        mutationFn: ({ deviceId, data }: { deviceId: string; data: Device.ReplaceSharesDto }) =>
            api.replaceDeviceShares(deviceId, data),
        successMessage: '设备分享已更新',
        invalidateKeys: [shareKeys.all, deviceKeys.all],
    });
}
export function useDeviceGroupShares(
    groupId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useQuery({
        queryKey: shareKeys.list('group', groupId ?? ''),
        queryFn: ({ signal }) => api.getDeviceGroupShares(groupId as string, signal),
        enabled: !!groupId && (options?.enabled ?? true),
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useDeviceGroupShareTargets(
    groupId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useQuery({
        queryKey: shareKeys.targets('group', groupId ?? ''),
        queryFn: ({ signal }) => api.getDeviceGroupShareTargets(groupId as string, signal),
        enabled: !!groupId && (options?.enabled ?? true),
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useReplaceDeviceGroupShares() {
    return useMutationWithMessage({
        mutationFn: ({ groupId, data }: { groupId: string; data: Device.ReplaceSharesDto }) =>
            api.replaceDeviceGroupShares(groupId, data),
        successMessage: '设备分组分享已更新',
        invalidateKeys: [shareKeys.all, deviceKeys.all, groupKeys.all],
    });
}
export function useDeviceGroupTree(
    options?: Omit<UseQueryOptions<DeviceGroup.TreeItem[]>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: [...groupKeys.all, 'tree'],
        queryFn: ({ signal }) => api.getDeviceGroups(signal).then(buildDeviceGroupTree),
        ...options,
        refetchInterval: false,
        refetchOnWindowFocus: false,
    });
}
export function useDeviceGroupTreeWithCount(
    options?: Omit<UseQueryOptions<DeviceGroup.TreeItem[]>, 'queryKey' | 'queryFn'>,
    debugDeviceId?: string,
    commandIds?: string[]
) {
    return useSnapshotQuery({
        queryKey: [...groupKeys.all, 'tree-count', debugDeviceId, commandIds],
        queryFn: () =>
            api.getDeviceGroupsWithCount(debugDeviceId, commandIds).map(buildDeviceGroupTree),
        placeholderData: keepPreviousData,
        ...options,
    });
}
export function useDeviceGroupSave() {
    return useSaveMutation<
        DeviceGroup.CreateDto & {
            id?: string;
        },
        DeviceGroup.CreateDto,
        DeviceGroup.UpdateDto
    >({
        createFn: api.createDeviceGroup,
        updateFn: api.updateDeviceGroup,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [groupKeys.all],
    });
}
export function useDeviceGroupDelete() {
    return useMutationWithMessage({
        mutationFn: api.removeDeviceGroup,
        successMessage: '删除成功',
        invalidateKeys: [groupKeys.all],
    });
}
export { getDeviceDetail, getDeviceOptions } from './device.api';

export function useDeviceDebug(id: string, open: boolean, commandIds?: string[]) {
    const packets = useSnapshotQuery({
        queryKey: ['device-debug-packets', id, commandIds],
        queryFn: () => getDebugPackets(id, commandIds),
        structuralSharing: retainNewerRecords,
        enabled: open,
    });
    const toggle = useMutationWithMessage({
        mutationFn: (enabled: boolean) => saveDebugSwitch(id, enabled),
        successMessage: '调试设置已保存',
        invalidateKeys: [deviceKeys.all],
    });
    return { packets, toggle };
}
