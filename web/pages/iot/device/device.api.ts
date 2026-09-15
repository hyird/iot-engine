import request from '@/lib/http';
import type { DebugAcquisition } from '@/types/packet_debug';
import type { PaginatedResult } from '@/types/pagination';
import { appendQueryParams } from '@/utils/query';
import { createSnapshotStream } from '@/lib/snapshot-request';
import { SnapshotStream } from '@/lib/snapshot-stream';
import {
    deviceCommandSchema,
    deviceDebugSchema,
    deviceIdSchema,
    replaceDeviceSharesSchema,
    saveDeviceGroupSchema,
    saveDeviceSchema,
} from './device.schema';
import type { Device, DeviceGroup } from './device.types';

const DEVICE_BASE = '/v1/device';
const GROUP_BASE = '/v1/device/groups';
export const setDebug = (id: string, enabled: boolean) =>
    request.put<void>(
        `${DEVICE_BASE}/${deviceIdSchema.parse(id)}/debug`,
        deviceDebugSchema.parse({ enabled })
    );
export const getDebugPackets = (id: string) =>
    createSnapshotStream<DebugAcquisition[]>(
        `${DEVICE_BASE}/${deviceIdSchema.parse(id)}/debug/packets`
    );

export const getDeviceList = () =>
    createSnapshotStream<PaginatedResult<Device.Overview>>(DEVICE_BASE);
export const getDeviceRealtimeSnapshot = () =>
    createSnapshotStream<PaginatedResult<Device.RealtimeSnapshot>>(`${DEVICE_BASE}/realtime`);
export const getDeviceDetail = (id: string) =>
    createSnapshotStream<Device.Overview>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`);
export const getDeviceHistory = (id: string, query: Device.HistoryRecordQuery) =>
    createSnapshotStream<PaginatedResult<Device.HistoryRecord>>(
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
        {
            ...deviceCommandSchema.parse(data),
            idempotency_key: data.idempotency_key ?? crypto.randomUUID(),
        }
    );
export const getDeviceCommandStatus = (id: string) =>
    createSnapshotStream<Device.CommandStatusResult>(
        `${DEVICE_BASE}/commands/${deviceIdSchema.parse(id)}`,
        {
            _silent: true,
        }
    );
export const getDeviceCommandStatuses = (commandIds: string[]) => {
    const ids = [...new Set(commandIds.map((id) => deviceIdSchema.parse(id)))];
    if (!ids.length)
        return SnapshotStream.value<Device.CommandStatusesResult>({ complete: true, statuses: [] });
    // Keep each request line below common reverse-proxy limits, including URL encoding.
    const batches: SnapshotStream<Device.CommandStatusesResult>[] = [];
    for (let offset = 0; offset < ids.length; offset += 64) {
        batches.push(
            createSnapshotStream<Device.CommandStatusesResult>(
                appendQueryParams(`${DEVICE_BASE}/commands`, {
                    ids: ids.slice(offset, offset + 64).join(','),
                }),
                { _silent: true }
            )
        );
    }
    return SnapshotStream.combine(batches).map((snapshots) => ({
        complete: snapshots.every((snapshot) => snapshot.complete),
        statuses: snapshots.flatMap((snapshot) => snapshot.statuses),
    }));
};
export const getDeviceShares = (id: string) =>
    createSnapshotStream<Device.ShareItem[]>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}/shares`);
export const getDeviceShareTargets = (id: string) =>
    createSnapshotStream<Device.ShareTarget[]>(
        `${DEVICE_BASE}/${deviceIdSchema.parse(id)}/share-targets`
    );
export const replaceDeviceShares = (id: string, data: Device.ReplaceSharesDto) =>
    request.put<void>(
        `${DEVICE_BASE}/${deviceIdSchema.parse(id)}/shares`,
        replaceDeviceSharesSchema.parse(data)
    );
export const getDeviceGroups = (withCount = false) =>
    createSnapshotStream<DeviceGroup.TreeItem[]>(
        `${GROUP_BASE}/${withCount ? 'tree-count' : 'tree'}`
    );
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
    createSnapshotStream<Device.ShareItem[]>(`${GROUP_BASE}/${deviceIdSchema.parse(id)}/shares`);
export const getDeviceGroupShareTargets = (id: string) =>
    createSnapshotStream<Device.ShareTarget[]>(
        `${GROUP_BASE}/${deviceIdSchema.parse(id)}/share-targets`
    );
export const replaceDeviceGroupShares = (id: string, data: Device.ReplaceSharesDto) =>
    request.put<void>(
        `${GROUP_BASE}/${deviceIdSchema.parse(id)}/shares`,
        replaceDeviceSharesSchema.parse(data)
    );
