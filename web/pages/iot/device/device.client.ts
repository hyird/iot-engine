import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import { consumeServerSentEvents } from '@/utils/sse';
import type { PaginatedResult } from '@/utils/types';
import { useAuthStore } from '@/store/authStore';
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

export const getDeviceList = () => request.get<PaginatedResult<Device.RealTimeData>>(DEVICE_BASE);
export const getDeviceRealtime = () =>
    request.get<PaginatedResult<Device.Realtime>>(`${DEVICE_BASE}/realtime`);
export async function subscribeDeviceRealtime(
    onEvent: (event: 'ready' | 'realtime') => void,
    signal: AbortSignal
) {
    let refreshed = false;
    for (;;) {
        const token = useAuthStore.getState().token;
        if (!token) throw new Error('登录状态已失效');

        const response = await fetch(`${DEVICE_BASE}/realtime/events`, {
            cache: 'no-store',
            credentials: 'same-origin',
            headers: {
                Accept: 'text/event-stream',
                Authorization: `Bearer ${token}`,
            },
            signal,
        });
        if (response.status === 401 && !refreshed) {
            refreshed = true;
            if (await useAuthStore.getState().refreshAccessToken()) continue;
        }
        if (!response.ok) throw new Error(`设备实时事件连接失败（HTTP ${response.status}）`);
        if (!response.body) throw new Error('浏览器不支持设备实时事件流');
        const contentType = response.headers.get('content-type') ?? '';
        if (!contentType.includes('text/event-stream')) throw new Error('设备实时事件响应格式错误');

        await consumeServerSentEvents(
            response.body,
            (event) => {
                if (event.event === 'ready' || event.event === 'realtime') onEvent(event.event);
            },
            signal
        );
        return;
    }
}
export const getDeviceDetail = (id: string) =>
    request.get<Device.RealTimeData>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}`);
export const getDeviceHistory = (id: string, query: Device.HistoryRecordQuery) =>
    request.get<PaginatedResult<Device.HistoryRecord>>(
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
        deviceCommandSchema.parse(data)
    );
export const getDeviceCommandStatus = (id: string) =>
    request.get<Device.CommandStatusResult>(`${DEVICE_BASE}/commands/${deviceIdSchema.parse(id)}`, {
        _silent: true,
    });
export const waitForDeviceCommands = (commandIds: string[], timeoutMs = 60_000) =>
    request.post<Device.CommandWaitResult>(
        `${DEVICE_BASE}/commands/wait`,
        { command_ids: commandIds.map((id) => deviceIdSchema.parse(id)), timeout_ms: timeoutMs },
        { timeout: timeoutMs + 5_000, _silent: true }
    );
export const getDeviceShares = (id: string) =>
    request.get<Device.ShareItem[]>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}/shares`);
export const getDeviceShareTargets = (id: string) =>
    request.get<Device.ShareTarget[]>(`${DEVICE_BASE}/${deviceIdSchema.parse(id)}/share-targets`);
export const replaceDeviceShares = (id: string, data: Device.ReplaceSharesDto) =>
    request.put<void>(
        `${DEVICE_BASE}/${deviceIdSchema.parse(id)}/shares`,
        replaceDeviceSharesSchema.parse(data)
    );

export const getDeviceGroupTree = async (withCount = false) =>
    buildTree(
        await request.get<DeviceGroup.TreeItem[]>(
            `${GROUP_BASE}/${withCount ? 'tree-count' : 'tree'}`
        )
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
    request.get<Device.ShareItem[]>(`${GROUP_BASE}/${deviceIdSchema.parse(id)}/shares`);
export const getDeviceGroupShareTargets = (id: string) =>
    request.get<Device.ShareTarget[]>(`${GROUP_BASE}/${deviceIdSchema.parse(id)}/share-targets`);
export const replaceDeviceGroupShares = (id: string, data: Device.ReplaceSharesDto) =>
    request.put<void>(
        `${GROUP_BASE}/${deviceIdSchema.parse(id)}/shares`,
        replaceDeviceSharesSchema.parse(data)
    );
