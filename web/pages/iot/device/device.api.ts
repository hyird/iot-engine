import request from '@/utils/http';
import type { PaginatedResult } from '@/utils/types';
import type { DeviceGroup } from './device-group.types';
import { deviceIdSchema, saveDeviceGroupSchema, saveDeviceSchema } from './device.schema';
import type { Device } from './device.types';

const DEVICE_BASE = '/api/device';
const GROUP_BASE = '/api/device-groups';

const buildTree = (items: DeviceGroup.TreeItem[]) => {
    const map = new Map<number, DeviceGroup.TreeItem>();
    const roots: DeviceGroup.TreeItem[] = [];
    for (const item of items) map.set(item.id, { ...item, children: [] });
    for (const item of map.values()) {
        const parent = item.parent_id ? map.get(item.parent_id) : undefined;
        if (parent) parent.children?.push(item);
        else roots.push(item);
    }
    return roots;
};

export const getDeviceList = () => request.get<PaginatedResult<Device.RealTimeData>>(DEVICE_BASE);
export const getDeviceDetail = (id: number) =>
    request.get<Device.Item>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`);
export const createDevice = (data: Device.CreateDto) =>
    request.post<void>(DEVICE_BASE, saveDeviceSchema.parse(data));
export const updateDevice = (id: number, data: Device.UpdateDto) =>
    request.put<void>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`, saveDeviceSchema.parse(data));
export const removeDevice = (id: number) =>
    request.delete<void>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`);

export const getDeviceGroupTree = async (withCount = false) =>
    buildTree(
        await request.get<DeviceGroup.TreeItem[]>(
            `${GROUP_BASE}/${withCount ? 'tree-count' : 'tree'}`
        )
    );
export const createDeviceGroup = (data: DeviceGroup.CreateDto) =>
    request.post<void>(GROUP_BASE, saveDeviceGroupSchema.parse(data));
export const updateDeviceGroup = (id: number, data: DeviceGroup.UpdateDto) =>
    request.put<void>(
        `${GROUP_BASE}/${deviceIdSchema.parse(id)}`,
        saveDeviceGroupSchema.parse(data)
    );
export const removeDeviceGroup = (id: number) =>
    request.delete<void>(`${GROUP_BASE}/${deviceIdSchema.parse(id)}`);
