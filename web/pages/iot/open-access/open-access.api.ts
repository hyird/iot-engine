import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import {
    deviceOptionSchema,
    keyItemSchema,
    keySaveSchema,
    keySecretSchema,
    logPageSchema,
    logQuerySchema,
    webhookItemSchema,
    webhookSaveSchema,
    openAccessIdSchema,
} from './open-access.schema';
import type { OpenAccess } from './open-access.types';

const BASE = '/api/open-access';

export const getDevices = async () =>
    deviceOptionSchema.array().parse(await request.get<unknown>(`${BASE}/devices`));
export const getKeys = async () =>
    keyItemSchema.array().parse(await request.get<unknown>(`${BASE}/keys`));
export const createKey = async (data: OpenAccess.KeySaveDto) =>
    keySecretSchema.parse(await request.post<unknown>(`${BASE}/keys`, keySaveSchema.parse(data)));
export const updateKey = (id: string, data: OpenAccess.KeySaveDto) =>
    request.put<void>(`${BASE}/keys/${openAccessIdSchema.parse(id)}`, keySaveSchema.parse(data));
export const rotateKey = async (id: string) =>
    keySecretSchema.parse(
        await request.post<unknown>(`${BASE}/keys/${openAccessIdSchema.parse(id)}/rotate`)
    );
export const deleteKey = (id: string) =>
    request.delete<void>(`${BASE}/keys/${openAccessIdSchema.parse(id)}`);

export const getWebhooks = async (accessKeyId?: string) =>
    webhookItemSchema
        .array()
        .parse(
            await request.get<unknown>(
                appendQueryParams(`${BASE}/webhooks`, accessKeyId ? { accessKeyId } : {})
            )
        );
export const createWebhook = (data: OpenAccess.WebhookSaveDto) =>
    request.post<void>(`${BASE}/webhooks`, webhookSaveSchema.parse(data));
export const updateWebhook = (id: string, data: OpenAccess.WebhookSaveDto) =>
    request.put<void>(
        `${BASE}/webhooks/${openAccessIdSchema.parse(id)}`,
        webhookSaveSchema.parse(data)
    );
export const deleteWebhook = (id: string) =>
    request.delete<void>(`${BASE}/webhooks/${openAccessIdSchema.parse(id)}`);

export const getLogs = async (query: OpenAccess.LogQuery) =>
    logPageSchema.parse(
        await request.get<unknown>(appendQueryParams(`${BASE}/logs`, logQuerySchema.parse(query)))
    );
