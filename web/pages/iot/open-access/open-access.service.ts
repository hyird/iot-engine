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
} from './open-access.client';
import { type Access, accessQueryKeys } from './open-access.types';

export const useOpenDevices = () =>
    useQuery({ queryKey: accessQueryKeys.devices(), queryFn: getDevices });
export const useOpenKeys = () =>
    useQuery({ queryKey: accessQueryKeys.keys(), queryFn: getKeys });
export const useOpenWebhooks = (accessKeyId?: string) =>
    useQuery({
        queryKey: accessQueryKeys.webhooks(accessKeyId),
        queryFn: () => getWebhooks(accessKeyId),
    });
export const useOpenLogs = (query: Access.LogQuery) =>
    useQuery({ queryKey: accessQueryKeys.logs(query), queryFn: () => getLogs(query) });

export const useKeyCreate = () =>
    useMutationWithMessage({
        mutationFn: createKey,
        successMessage: '调用配置已创建，请立即保存 AccessKey',
        invalidateKeys: [accessQueryKeys.all],
    });
export const useKeyUpdate = () =>
    useMutationWithMessage({
        mutationFn: ({ id, data }: { id: string; data: Access.KeySaveDto }) =>
            updateKey(id, data),
        successMessage: '调用配置已更新',
        invalidateKeys: [accessQueryKeys.all],
    });
export const useKeyRotate = () =>
    useMutationWithMessage({
        mutationFn: rotateKey,
        successMessage: 'AccessKey 已轮换，旧密钥立即失效',
        invalidateKeys: [accessQueryKeys.all],
    });
export const useKeyDelete = () =>
    useMutationWithMessage({
        mutationFn: deleteKey,
        successMessage: '调用配置已删除',
        invalidateKeys: [accessQueryKeys.all],
    });
export const useWebhookCreate = () =>
    useMutationWithMessage({
        mutationFn: createWebhook,
        successMessage: 'Webhook 已创建',
        invalidateKeys: [accessQueryKeys.all],
    });
export const useWebhookUpdate = () =>
    useMutationWithMessage({
        mutationFn: ({ id, data }: { id: string; data: Access.WebhookSaveDto }) =>
            updateWebhook(id, data),
        successMessage: 'Webhook 已更新',
        invalidateKeys: [accessQueryKeys.all],
    });
export const useWebhookDelete = () =>
    useMutationWithMessage({
        mutationFn: deleteWebhook,
        successMessage: 'Webhook 已删除',
        invalidateKeys: [accessQueryKeys.all],
    });
