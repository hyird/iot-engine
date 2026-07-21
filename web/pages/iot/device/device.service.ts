import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { createQueryKeys } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import type { DeviceGroup } from './device-group.types';
import * as api from './device.api';
import type { Device } from './device.types';

const deviceKeys = createQueryKeys('devices');
const groupKeys = createQueryKeys('device-groups');
const EMPTY_AGENTS: AgentOption[] = [];
const EMPTY_ENDPOINTS: AgentEndpoint[] = [];

interface AgentOption {
    id: number;
    name: string;
    code: string;
    is_online: boolean;
}

interface AgentEndpoint {
    id: number;
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
    });
}

export function useDeviceSave() {
    return useSaveMutation<Device.CreateDto & { id?: number }, Device.CreateDto, Device.UpdateDto>({
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
        DeviceGroup.CreateDto & { id?: number },
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

export function useAgentEndpoints(agentId?: number, options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: ['agents', agentId, 'endpoints'],
        queryFn: async () => EMPTY_ENDPOINTS,
        enabled: options?.enabled ?? !!agentId,
    });
}

export type DeviceListResult = PaginatedResult<Device.RealTimeData>;
