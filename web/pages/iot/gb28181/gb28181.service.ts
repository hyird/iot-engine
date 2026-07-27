import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import * as api from './gb28181.client';
import type { GB28181 } from './gb28181.types';

export { api as gb28181Api };
export { sendPtz, sendPtzPosition, stopPreviewKeepalive } from './gb28181.client';

export const gb28181Keys = {
    all: ['gb28181'] as const,
    health: () => ['gb28181', 'health'] as const,
    devices: () => ['gb28181', 'devices'] as const,
    streams: () => ['gb28181', 'streams'] as const,
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
