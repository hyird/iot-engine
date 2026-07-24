import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { createQueryKeys } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import * as api from './device.client';
import type { Device } from './device.types';
import type { DeviceGroup } from './device-group.types';

const deviceKeys = createQueryKeys('devices');
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

export function useDeviceList(options?: { enabled?: boolean; pollingInterval?: number | false }) {
    return useQuery({
        queryKey: deviceKeys.lists(),
        queryFn: api.getDeviceList,
        enabled: options?.enabled ?? true,
        refetchInterval: options?.pollingInterval ?? false,
        refetchOnWindowFocus: false,
        staleTime: 5_000,
    });
}

export function useDeviceHistory(
    deviceId: string | undefined,
    query: Device.HistoryRecordQuery,
    enabled = true
) {
    return useQuery({
        queryKey: [...deviceKeys.all, 'history', deviceId ?? '', query],
        queryFn: () => api.getDeviceHistory(deviceId as string, query),
        enabled: Boolean(deviceId) && enabled,
    });
}

export function useDeviceSave() {
    return useSaveMutation<Device.CreateDto & { id?: string }, Device.CreateDto, Device.UpdateDto>({
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

const waitForCommand = async (commandId: string) => {
    const deadline = Date.now() + 60_000;
    while (Date.now() < deadline) {
        const state = await api.getDeviceCommandStatus(commandId);
        if (state.status === 'SUCCESS') return;
        if (state.status === 'FAILED') throw new Error(state.reason || '设备执行指令失败');
        await new Promise((resolve) => window.setTimeout(resolve, 150));
    }
    throw new Error('等待设备应答超时');
};

export function useDeviceCommand() {
    return useMutationWithMessage({
        mutationFn: async ({ deviceId, data }: { deviceId: string; data: Device.Command }) => {
            const command = await api.createDeviceCommand(deviceId, data);
            await Promise.all(command.command_ids.map(waitForCommand));
        },
        successMessage: '指令下发成功，设备已应答',
        errorMessage: (error) => error.message,
        invalidateKeys: [deviceKeys.all],
    });
}

export function useDeviceShares(deviceId?: string, options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: shareKeys.list('device', deviceId ?? ''),
        queryFn: () => api.getDeviceShares(deviceId as string),
        enabled: !!deviceId && (options?.enabled ?? true),
    });
}

export function useDeviceShareTargets(deviceId?: string, options?: { enabled?: boolean }) {
    return useQuery({
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

export function useDeviceGroupShares(groupId?: string, options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: shareKeys.list('group', groupId ?? ''),
        queryFn: () => api.getDeviceGroupShares(groupId as string),
        enabled: !!groupId && (options?.enabled ?? true),
    });
}

export function useDeviceGroupShareTargets(groupId?: string, options?: { enabled?: boolean }) {
    return useQuery({
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
    return useQuery({
        queryKey: [...groupKeys.all, 'tree'],
        queryFn: () => api.getDeviceGroupTree(false),
        ...options,
    });
}

export function useDeviceGroupTreeWithCount(
    options?: Omit<UseQueryOptions<DeviceGroup.TreeItem[]>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: [...groupKeys.all, 'tree-count'],
        queryFn: () => api.getDeviceGroupTree(true),
        ...options,
    });
}

export function useDeviceGroupSave() {
    return useSaveMutation<
        DeviceGroup.CreateDto & { id?: string },
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
    return useQuery({
        queryKey: ['agents', 'options'],
        queryFn: async () => EMPTY_AGENTS,
        enabled: options?.enabled ?? true,
    });
}

export function useAgentEndpoints(agentId?: string, options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: ['agents', agentId, 'endpoints'],
        queryFn: async () => EMPTY_ENDPOINTS,
        enabled: options?.enabled ?? !!agentId,
    });
}

export type DeviceListResult = PaginatedResult<Device.RealTimeData>;
