import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import * as api from './gb28181.client';
import type { GB28181 } from './gb28181.types';

export { api as gb28181Api };
export { renewPreview, sendPtz, sendPtzPosition, stopPreviewKeepalive } from './gb28181.client';

export const gb28181Keys = {
    all: ['gb28181'] as const,
    health: () => ['gb28181', 'health'] as const,
    devices: () => ['gb28181', 'devices'] as const,
    streams: () => ['gb28181', 'streams'] as const,
    recording: (streamId: string) => ['gb28181', 'streams', streamId, 'recording'] as const,
};

export function useGb28181Health(
    options?: Omit<UseQueryOptions<GB28181.Health>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: gb28181Keys.health(),
        queryFn: api.getHealth,
        refetchInterval: 10_000,
        retry: false,
        ...options,
    });
}

export function useGb28181Devices(
    options?: Omit<UseQueryOptions<GB28181.Items<GB28181.Device>>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: gb28181Keys.devices(),
        queryFn: api.getDevices,
        refetchInterval: 3_000,
        refetchIntervalInBackground: false,
        ...options,
    });
}

export function useGb28181CatalogQuery() {
    return useMutationWithMessage({
        mutationFn: api.queryCatalog,
        successMessage: '目录查询已发送',
        invalidateKeys: [gb28181Keys.devices()],
    });
}

export function useGb28181RenameDevice() {
    return useMutationWithMessage<void, GB28181.DeviceNamePayload>({
        mutationFn: api.renameDevice,
        successMessage: '摄像头名称已更新',
        invalidateKeys: [gb28181Keys.devices()],
    });
}

export function useGb28181RenameChannel() {
    return useMutationWithMessage<void, GB28181.ChannelNamePayload>({
        mutationFn: api.renameChannel,
        successMessage: '通道名称已更新',
        invalidateKeys: [gb28181Keys.devices()],
    });
}

export function useGb28181PreviewStart() {
    return useMutationWithMessage<GB28181.PreviewStartResult, GB28181.StartPreviewPayload>({
        mutationFn: api.startPreview,
        successMessage: '预览已发起',
        invalidateKeys: [gb28181Keys.streams()],
    });
}

export function useGb28181PreviewStop() {
    return useMutationWithMessage<GB28181.PreviewStopResult, GB28181.StopPreviewPayload>({
        mutationFn: api.stopPreview,
        successMessage: '会话已停止',
        invalidateKeys: [gb28181Keys.streams()],
    });
}

export function useGb28181Recording(streamId?: string, enabled = true) {
    return useQuery({
        queryKey: gb28181Keys.recording(streamId ?? ''),
        queryFn: () => api.getRecording({ streamId: streamId ?? '' }),
        enabled: enabled && Boolean(streamId),
        refetchInterval: 3_000,
    });
}

export function useGb28181RecordingStart() {
    return useMutationWithMessage<GB28181.CommandResult, GB28181.StreamPayload>({
        mutationFn: api.startRecording,
        successMessage: '录像已开始',
        invalidateKeys: [gb28181Keys.streams()],
    });
}

export function useGb28181RecordingStop() {
    return useMutationWithMessage<GB28181.CommandResult, GB28181.StreamPayload>({
        mutationFn: api.stopRecording,
        successMessage: '录像已停止',
        invalidateKeys: [gb28181Keys.streams()],
    });
}
