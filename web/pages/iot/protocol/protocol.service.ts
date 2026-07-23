import { type UseQueryOptions, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { createQueryKeys } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import * as api from './protocol.client';
import type { Protocol } from './protocol.types';

export const protocolQueryKeys = {
    ...createQueryKeys('protocol-configs'),
    list: (params?: Protocol.Query) => ['protocol-configs', 'list', params] as const,
};

export type SaveProtocolConfigParams =
    | (Protocol.CreateDto & { id?: undefined })
    | (Protocol.UpdateDto & { id: string; protocol?: Protocol.Type });

export function useProtocolConfigList(
    params?: Protocol.Query,
    options?: Omit<UseQueryOptions<Protocol.Item[]>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: protocolQueryKeys.list(params),
        queryFn: () => api.getAll(params),
        ...options,
    });
}

export function useProtocolConfigOptions(
    protocol: Protocol.Type,
    options?: Omit<UseQueryOptions<PaginatedResult<Protocol.Option>>, 'queryKey' | 'queryFn'>
) {
    return useQuery({
        queryKey: [...protocolQueryKeys.all, 'options', protocol],
        queryFn: () => api.getOptions(protocol),
        ...options,
    });
}

export function useProtocolConfigSave() {
    return useSaveMutation<SaveProtocolConfigParams, Protocol.CreateDto, Protocol.UpdateDto>({
        createFn: api.create,
        updateFn: api.update,
        toUpdatePayload: (data) => {
            const { id: _id, protocol: _protocol, ...rest } = data;
            return rest as Protocol.UpdateDto;
        },
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [protocolQueryKeys.all],
    });
}

export function useProtocolConfigDelete() {
    return useMutationWithMessage({
        mutationFn: api.remove,
        successMessage: '删除成功',
        invalidateKeys: [protocolQueryKeys.all],
    });
}
