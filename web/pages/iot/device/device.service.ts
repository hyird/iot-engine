import { retainNewerRecords } from '@/utils/versioned-records';
import type { UseQueryOptions } from '@tanstack/react-query';
import { setDebug as saveDebugSwitch, getDebugPackets } from './device.api';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import { parseDateTime } from '@/utils/dateTime';
import { createQueryKeys } from '@/utils/query';
import { SnapshotStream } from '@/lib/snapshot-stream';
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
const EMPTY_AGENTS: AgentOption[] = [];
const EMPTY_ENDPOINTS: AgentEndpoint[] = [];
interface AgentOption {
    id: string;
    name: string;
    code: string;
    is_online: boolean;
}
interface AgentEndpoint {
    id: string;
    name: string;
    protocol: string;
    mode: string;
    transport?: string;
    channel?: string;
    baud_rate?: number;
    ip?: string;
    port?: number;
}
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
export function useDeviceList(options?: { enabled?: boolean }) {
    return useSnapshotQuery({
        queryKey: deviceKeys.lists(),
        queryFn: api.getDeviceList,
        enabled: options?.enabled ?? true,
        refetchOnWindowFocus: false,
        staleTime: 5000,
    });
}
export function useDeviceRealtimeSnapshot(options?: { enabled?: boolean }) {
    return useSnapshotQuery({
        queryKey: deviceRealtimeSnapshotKey,
        queryFn: api.getDeviceRealtimeSnapshot,
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
    return useSnapshotQuery({
        queryKey: [...deviceKeys.all, 'history', deviceId ?? '', query],
        queryFn: () => api.getDeviceHistory(deviceId as string, query),
        enabled: Boolean(deviceId) && enabled,
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
        mutationFn: async ({ deviceId, data }: { deviceId: string; data: Device.Command }) => {
            const command = await api.createDeviceCommand(deviceId, data);
            const result = await api
                .getDeviceCommandStatuses(command.command_ids)
                .filter((snapshot) => snapshot.complete)
                .first(AbortSignal.timeout(60000));
            const failed = result.statuses.find((state) =>
                ['FAILED', 'REJECTED', 'UNKNOWN', 'READBACK_MISMATCH'].includes(state.status)
            );
            if (failed)
                throw new Error(
                    failed.status === 'UNKNOWN'
                        ? '指令结果未知，请核对设备状态，勿直接重发'
                        : failed.reason || '设备执行指令失败'
                );
            if (!result.complete) throw new Error('等待设备应答超时');
            return result;
        },
        successMessage: (result) => {
            const actualValues = result.statuses.flatMap((status) => status.actual_values ?? []);
            if (!actualValues.length) return '指令下发成功，设备已应答';
            const readback = actualValues
                .map(
                    (actual) =>
                        `${actual.name || actual.element_id}=${actual.value}${actual.unit || ''}`
                )
                .join('，');
            return `指令下发成功，设备回读：${readback}`;
        },
        errorMessage: (error) => error.message,
        invalidateKeys: [deviceKeys.all],
    });
}
export function useDeviceShares(
    deviceId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useSnapshotQuery({
        queryKey: shareKeys.list('device', deviceId ?? ''),
        queryFn: () => api.getDeviceShares(deviceId as string),
        enabled: !!deviceId && (options?.enabled ?? true),
    });
}
export function useDeviceShareTargets(
    deviceId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useSnapshotQuery({
        queryKey: shareKeys.targets('device', deviceId ?? ''),
        queryFn: () => api.getDeviceShareTargets(deviceId as string),
        enabled: !!deviceId && (options?.enabled ?? true),
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
    return useSnapshotQuery({
        queryKey: shareKeys.list('group', groupId ?? ''),
        queryFn: () => api.getDeviceGroupShares(groupId as string),
        enabled: !!groupId && (options?.enabled ?? true),
    });
}
export function useDeviceGroupShareTargets(
    groupId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useSnapshotQuery({
        queryKey: shareKeys.targets('group', groupId ?? ''),
        queryFn: () => api.getDeviceGroupShareTargets(groupId as string),
        enabled: !!groupId && (options?.enabled ?? true),
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
    return useSnapshotQuery({
        queryKey: [...groupKeys.all, 'tree'],
        queryFn: () => api.getDeviceGroups(false).map(buildDeviceGroupTree),
        ...options,
    });
}
export function useDeviceGroupTreeWithCount(
    options?: Omit<UseQueryOptions<DeviceGroup.TreeItem[]>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: [...groupKeys.all, 'tree-count'],
        queryFn: () => api.getDeviceGroups(true).map(buildDeviceGroupTree),
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
export function useAgentOptions(options?: { enabled?: boolean }) {
    return useSnapshotQuery({
        queryKey: ['agents', 'options'],
        queryFn: () => SnapshotStream.value(EMPTY_AGENTS),
        enabled: options?.enabled ?? true,
    });
}
export function useAgentEndpoints(
    agentId?: string,
    options?: {
        enabled?: boolean;
    }
) {
    return useSnapshotQuery({
        queryKey: ['agents', agentId, 'endpoints'],
        queryFn: () => SnapshotStream.value(EMPTY_ENDPOINTS),
        enabled: options?.enabled ?? !!agentId,
    });
}

export { getDeviceDetail } from './device.api';

export function useDeviceDebug(id: string, open: boolean) {
    const packets = useSnapshotQuery({
        queryKey: ['device-debug-packets', id],
        queryFn: () => getDebugPackets(id),
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
