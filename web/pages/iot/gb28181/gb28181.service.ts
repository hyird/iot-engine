import type { UseQueryOptions } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import * as api from './gb28181.api';
import type { GB28181, PlaybackCandidate, PlaybackCapabilities } from './gb28181.types';
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
    return useSnapshotQuery({
        queryKey: gb28181Keys.health(),
        queryFn: api.getHealth,
        retry: false,
        ...options,
    });
}
export function useGb28181Devices(
    options?: Omit<UseQueryOptions<GB28181.Items<GB28181.Device>>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: gb28181Keys.devices(),
        queryFn: api.getDevices,
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
    return useSnapshotQuery({
        queryKey: gb28181Keys.recording(streamId ?? ''),
        queryFn: () => api.getRecording({ streamId: streamId ?? '' }),
        enabled: enabled && Boolean(streamId),
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

const compact = (candidates: Array<PlaybackCandidate | null>) =>
    candidates.filter((candidate): candidate is PlaybackCandidate => Boolean(candidate?.url));
export function buildPlaybackCandidates(
    urls: GB28181.PlayUrls,
    capabilities: PlaybackCapabilities
): PlaybackCandidate[] {
    const nativeFlv = capabilities.webCodecs
        ? compact([
              urls.ws_flv
                  ? {
                        decoder: 'native-only',
                        engine: 'adaptive-flv',
                        label: 'WS-FLV · WebCodecs',
                        url: urls.ws_flv,
                    }
                  : null,
              urls.http_flv
                  ? {
                        decoder: 'native-only',
                        engine: 'adaptive-flv',
                        label: 'HTTP-FLV · WebCodecs',
                        url: urls.http_flv,
                    }
                  : null,
          ])
        : [];
    const mse = capabilities.mpegts
        ? compact([
              urls.http_ts
                  ? {
                        engine: 'mpegts',
                        label: 'HTTP-TS · 原生解码',
                        mediaType: 'mpegts',
                        url: urls.http_ts,
                    }
                  : null,
              urls.ws_flv
                  ? {
                        engine: 'mpegts',
                        label: 'WS-FLV · 原生解码',
                        mediaType: 'flv',
                        url: urls.ws_flv,
                    }
                  : null,
              urls.http_flv
                  ? {
                        engine: 'mpegts',
                        label: 'HTTP-FLV · 原生解码',
                        mediaType: 'flv',
                        url: urls.http_flv,
                    }
                  : null,
          ])
        : [];
    const softwareFlv = capabilities.softwareVideo
        ? compact([
              urls.ws_flv
                  ? {
                        decoder: 'software-only',
                        engine: 'adaptive-flv',
                        label: 'WS-FLV · Worker软解',
                        url: urls.ws_flv,
                    }
                  : null,
              urls.http_flv
                  ? {
                        decoder: 'software-only',
                        engine: 'adaptive-flv',
                        label: 'HTTP-FLV · Worker软解',
                        url: urls.http_flv,
                    }
                  : null,
          ])
        : [];
    const ordered = capabilities.mseH265
        ? [...mse.slice(0, 1), ...nativeFlv, ...mse.slice(1), ...softwareFlv]
        : [...nativeFlv, ...mse, ...softwareFlv];
    if (capabilities.hls && urls.hls) {
        ordered.push({ engine: 'hls', label: 'HLS', url: urls.hls });
    }
    return ordered;
}

export { renewPreview, sendPtz, sendPtzPosition, stopPreviewKeepalive } from './gb28181.api';
