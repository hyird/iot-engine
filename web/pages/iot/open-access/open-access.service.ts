import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import {
    createKey,
    createWebhook,
    deleteKey,
    deleteWebhook,
    getDevices,
    getKeys,
    getLogs,
    getWebhooks,
    rotateKey,
    updateKey,
    updateWebhook,
} from './open-access.api';
import { type OpenAccess, openAccessQueryKeys } from './open-access.types';

export const useOpenDevices = () =>
    useQuery({ queryKey: openAccessQueryKeys.devices(), queryFn: getDevices });
export const useOpenKeys = () =>
    useQuery({ queryKey: openAccessQueryKeys.keys(), queryFn: getKeys });
export const useOpenWebhooks = (accessKeyId?: string) =>
    useQuery({
        queryKey: openAccessQueryKeys.webhooks(accessKeyId),
        queryFn: () => getWebhooks(accessKeyId),
    });
export const useOpenLogs = (query: OpenAccess.LogQuery) =>
    useQuery({ queryKey: openAccessQueryKeys.logs(query), queryFn: () => getLogs(query) });

export const useKeyCreate = () =>
    useMutationWithMessage({
        mutationFn: createKey,
        successMessage: '调用配置已创建，请立即保存 AccessKey',
        invalidateKeys: [openAccessQueryKeys.all],
    });
export const useKeyUpdate = () =>
    useMutationWithMessage({
        mutationFn: ({ id, data }: { id: string; data: OpenAccess.KeySaveDto }) =>
            updateKey(id, data),
        successMessage: '调用配置已更新',
        invalidateKeys: [openAccessQueryKeys.all],
    });
export const useKeyRotate = () =>
    useMutationWithMessage({
        mutationFn: rotateKey,
        successMessage: 'AccessKey 已轮换，旧密钥立即失效',
        invalidateKeys: [openAccessQueryKeys.all],
    });
export const useKeyDelete = () =>
    useMutationWithMessage({
        mutationFn: deleteKey,
        successMessage: '调用配置已删除',
        invalidateKeys: [openAccessQueryKeys.all],
    });
export const useWebhookCreate = () =>
    useMutationWithMessage({
        mutationFn: createWebhook,
        successMessage: 'Webhook 已创建',
        invalidateKeys: [openAccessQueryKeys.all],
    });
export const useWebhookUpdate = () =>
    useMutationWithMessage({
        mutationFn: ({ id, data }: { id: string; data: OpenAccess.WebhookSaveDto }) =>
            updateWebhook(id, data),
        successMessage: 'Webhook 已更新',
        invalidateKeys: [openAccessQueryKeys.all],
    });
export const useWebhookDelete = () =>
    useMutationWithMessage({
        mutationFn: deleteWebhook,
        successMessage: 'Webhook 已删除',
        invalidateKeys: [openAccessQueryKeys.all],
    });
