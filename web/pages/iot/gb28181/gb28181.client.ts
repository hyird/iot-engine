import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { GB28181 } from './gb28181.types';

const BASE = '/v1/gb28181';
const pathPart = (value: string) => encodeURIComponent(value);

export function stopPreviewKeepalive(sessionId: string, token?: string | null) {
    const headers = new Headers();
    if (token) headers.set('Authorization', `Bearer ${token}`);
    void fetch(`${BASE}/previews/${pathPart(sessionId)}/stop`, {
        method: 'POST',
        headers,
        credentials: 'same-origin',
        keepalive: true,
    }).catch(() => undefined);
}

export const getHealth = () => request.get<GB28181.Health>(`${BASE}/health`, { _silent: true });
export const getSipConfig = () => request.get<GB28181.SipConfig>(`${BASE}/config/sip`);
export const getDevices = () => request.get<GB28181.Items<GB28181.Device>>(`${BASE}/devices`);
export const getStreams = () => request.get<GB28181.Items<GB28181.StreamStatus>>(`${BASE}/streams`);
export const renameDevice = (payload: GB28181.DeviceNamePayload) =>
    request.put<void>(`${BASE}/devices/${pathPart(payload.deviceId)}/name`, {
        name: payload.name,
    });
export const renameChannel = (payload: GB28181.ChannelNamePayload) =>
    request.put<void>(
        `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/name`,
        { name: payload.name }
    );
export const queryCatalog = (deviceId: string) =>
    request.post<GB28181.CommandResult>(`${BASE}/devices/${pathPart(deviceId)}/catalog/query`);
export const startPreview = (payload: GB28181.StartPreviewPayload) =>
    request.post<GB28181.PreviewStartResult>(
        `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/preview/start`
    );
export const stopPreview = (payload: GB28181.StopPreviewPayload) =>
    request.post<GB28181.PreviewStopResult>(`${BASE}/previews/${pathPart(payload.sessionId)}/stop`);
export const sendPtz = (payload: GB28181.PtzPayload) =>
    request.post<GB28181.CommandResult>(
        appendQueryParams(
            `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/ptz/${payload.action}`,
            { speed: payload.speed }
        )
    );
export const sendPtzPosition = (payload: GB28181.PtzPositionPayload) =>
    request.post<GB28181.CommandResult>(
        appendQueryParams(
            `${BASE}/devices/${pathPart(payload.deviceId)}/channels/${pathPart(payload.channelId)}/ptz/position/set`,
            { pan: payload.pan, tilt: payload.tilt, zoom: payload.zoom }
        )
    );
export const getRecording = (payload: GB28181.StreamPayload) =>
    request.get<GB28181.CommandResult>(`${BASE}/streams/${pathPart(payload.streamId)}/recording`);
export const startRecording = (payload: GB28181.StreamPayload) =>
    request.post<GB28181.CommandResult>(
        `${BASE}/streams/${pathPart(payload.streamId)}/recording/start`
    );
export const stopRecording = (payload: GB28181.StreamPayload) =>
    request.post<GB28181.CommandResult>(
        `${BASE}/streams/${pathPart(payload.streamId)}/recording/stop`
    );
