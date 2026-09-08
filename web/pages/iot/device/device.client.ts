import { liveRead } from '@/utils/live-request';
import { LiveResource } from '@/utils/live-resource';
import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import {
    deviceCommandSchema,
    deviceIdSchema,
    replaceDeviceSharesSchema,
    saveDeviceGroupSchema,
    saveDeviceSchema,
} from './device.schema';
import type { Device } from './device.types';
import type { DeviceGroup } from './device-group.types';

const DEVICE_BASE = '/v1/device';
const GROUP_BASE = '/v1/device/groups';

const buildTree = (items: DeviceGroup.TreeItem[]) => {
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

export const getDeviceList = () => liveRead<PaginatedResult<Device.RealTimeData>>(DEVICE_BASE);
export const getDeviceRealtime = () =>
    liveRead<PaginatedResult<Device.Realtime>>(`${DEVICE_BASE}/realtime`);
export const getDeviceDetail = (id: string) =>
    liveRead<Device.RealTimeData>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`);
export const getDeviceHistory = (id: string, query: Device.HistoryRecordQuery) =>
    liveRead<PaginatedResult<Device.HistoryRecord>>(
        appendQueryParams(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}/history`, query)
    );
export const createDevice = (data: Device.CreateDto) =>
    request.post<void>(DEVICE_BASE, saveDeviceSchema.parse(data));
export const updateDevice = (id: string, data: Device.UpdateDto) =>
    request.put<void>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`, saveDeviceSchema.parse(data));
export const removeDevice = (id: string) =>
    request.delete<void>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`);
export const createDeviceCommand = (id: string, data: Device.Command) =>
    request.post<Device.CommandCreateResult>(
        `${DEVICE_BASE}/${deviceIdSchema.parse(id)}/commands`,
        { ...deviceCommandSchema.parse(data), idempotency_key: data.idempotency_key ?? crypto.randomUUID() }
    );
export const getDeviceCommandStatus = (id: string) =>
    liveRead<Device.CommandStatusResult>(`${DEVICE_BASE}/commands/${deviceIdSchema.parse(id)}`, {
        _silent: true,
    });
export const getDeviceCommandStatuses = (commandIds: string[]) => {
    const ids = [...new Set(commandIds.map((id) => deviceIdSchema.parse(id)))];
    if (!ids.length) return LiveResource.value<Device.CommandStatusesResult>({ complete: true, statuses: [] });
    // Keep each request line below common reverse-proxy limits, including URL encoding.
    const batches: LiveResource<Device.CommandStatusesResult>[] = [];
    for (let offset = 0; offset < ids.length; offset += 64) {
        batches.push(liveRead<Device.CommandStatusesResult>(appendQueryParams(
            `${DEVICE_BASE}/commands`, { ids: ids.slice(offset, offset + 64).join(',') }
        ), { _silent: true }));
    }
    return LiveResource.combine(batches).map((snapshots) => ({
        complete: snapshots.every((snapshot) => snapshot.complete),
        statuses: snapshots.flatMap((snapshot) => snapshot.statuses),
    }));
};
export const getDeviceShares = (id: string) =>
    liveRead<Device.ShareItem[]>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}/shares`);
export const getDeviceShareTargets = (id: string) =>
    liveRead<Device.ShareTarget[]>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}/share-targets`);
export const replaceDeviceShares = (id: string, data: Device.ReplaceSharesDto) =>
    request.put<void>(
        `${DEVICE_BASE}/${deviceIdSchema.parse(id)}/shares`,
        replaceDeviceSharesSchema.parse(data)
    );

export const getDeviceGroupTree = (withCount = false) =>
    liveRead<DeviceGroup.TreeItem[]>(
        `${GROUP_BASE}/${withCount ? 'tree-count' : 'tree'}`
    ).map(buildTree);
export const createDeviceGroup = (data: DeviceGroup.CreateDto) =>
    request.post<void>(GROUP_BASE, saveDeviceGroupSchema.parse(data));
export const updateDeviceGroup = (id: string, data: DeviceGroup.UpdateDto) =>
    request.put<void>(
        `${GROUP_BASE}/${deviceIdSchema.parse(id)}`,
        saveDeviceGroupSchema.parse(data)
    );
export const removeDeviceGroup = (id: string) =>
    request.delete<void>(`${GROUP_BASE}/${deviceIdSchema.parse(id)}`);
export const getDeviceGroupShares = (id: string) =>
    liveRead<Device.ShareItem[]>(`${GROUP_BASE}/${deviceIdSchema.parse(id)}/shares`);
export const getDeviceGroupShareTargets = (id: string) =>
    liveRead<Device.ShareTarget[]>(`${GROUP_BASE}/${deviceIdSchema.parse(id)}/share-targets`);
export const replaceDeviceGroupShares = (id: string, data: Device.ReplaceSharesDto) =>
    request.put<void>(
        `${GROUP_BASE}/${deviceIdSchema.parse(id)}/shares`,
        replaceDeviceSharesSchema.parse(data)
    );
