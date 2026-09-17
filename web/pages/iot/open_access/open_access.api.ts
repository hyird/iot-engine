import request from '@/lib/http';
import {
    accessIdSchema,
    deviceOptionSchema,
    keyItemSchema,
    keySaveSchema,
    keySecretSchema,
    logPageSchema,
    logQuerySchema,
    webhookItemSchema,
    webhookSaveSchema,
} from './open_access.schema';
import type { Access } from './open_access.types';

export const getDevices = async (signal?: AbortSignal) =>
    deviceOptionSchema.array().parse(await request.get<unknown>('/api/device/options', { signal }));
export const getKeys = async (signal?: AbortSignal) =>
    keyItemSchema.array().parse(await request.get<unknown>('/api/open-access-key', { signal }));
export const createKey = async (data: Access.KeySaveDto) =>
    keySecretSchema.parse(
        await request.post<unknown>('/api/open-access-key', keySaveSchema.parse(data))
    );
export const updateKey = (id: string, data: Access.KeySaveDto) =>
    request.put<void>(
        `/api/open-access-key/${accessIdSchema.parse(id)}`,
        keySaveSchema.parse(data)
    );
export const rotateKey = async (id: string) =>
    keySecretSchema.parse(
        await request.post<unknown>(`/api/open-access-key/${accessIdSchema.parse(id)}/rotate`)
    );
export const deleteKey = (id: string) =>
    request.delete<void>(`/api/open-access-key/${accessIdSchema.parse(id)}`);
export const getWebhooks = async (accessKeyId?: string, signal?: AbortSignal) =>
    webhookItemSchema.array().parse(
        await request.get<unknown>('/api/open-webhook', {
            params: accessKeyId ? { accessKeyId: accessIdSchema.parse(accessKeyId) } : {},
            signal,
        })
    );
export const createWebhook = (data: Access.WebhookSaveDto) =>
    request.post<void>('/api/open-webhook', webhookSaveSchema.parse(data));
export const updateWebhook = (id: string, data: Access.WebhookSaveDto) =>
    request.put<void>(
        `/api/open-webhook/${accessIdSchema.parse(id)}`,
        webhookSaveSchema.parse(data)
    );
export const deleteWebhook = (id: string) =>
    request.delete<void>(`/api/open-webhook/${accessIdSchema.parse(id)}`);
export const getLogs = async (query: Access.LogQuery, signal?: AbortSignal) =>
    logPageSchema.parse(
        await request.get<unknown>('/api/open-access-log', {
            params: logQuerySchema.parse(query),
            signal,
        })
    );
