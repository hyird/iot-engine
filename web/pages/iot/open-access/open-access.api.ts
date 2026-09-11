import { createSnapshotStream } from '@/utils/snapshot-request';
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
    accessIdSchema,
} from './open-access.schema';
import type { Access } from './open-access.types';

const DEVICE_OPTIONS = '/api/device/options';
const ACCESS_KEYS = '/api/open-access-key';
const WEBHOOKS = '/api/open-webhook';
const ACCESS_LOGS = '/api/open-access-log';

export const getDevices = () =>
    createSnapshotStream<unknown>(DEVICE_OPTIONS).map((value) =>
        deviceOptionSchema.array().parse(value)
    );
export const getKeys = () =>
    createSnapshotStream<unknown>(ACCESS_KEYS).map((value) => keyItemSchema.array().parse(value));
export const createKey = async (data: Access.KeySaveDto) =>
    keySecretSchema.parse(await request.post<unknown>(ACCESS_KEYS, keySaveSchema.parse(data)));
export const updateKey = (id: string, data: Access.KeySaveDto) =>
    request.put<void>(`${ACCESS_KEYS}/${accessIdSchema.parse(id)}`, keySaveSchema.parse(data));
export const rotateKey = async (id: string) =>
    keySecretSchema.parse(
        await request.post<unknown>(`${ACCESS_KEYS}/${accessIdSchema.parse(id)}/rotate`)
    );
export const deleteKey = (id: string) =>
    request.delete<void>(`${ACCESS_KEYS}/${accessIdSchema.parse(id)}`);

export const getWebhooks = (accessKeyId?: string) =>
    createSnapshotStream<unknown>(
        appendQueryParams(WEBHOOKS, accessKeyId ? { accessKeyId } : {})
    ).map((value) => webhookItemSchema.array().parse(value));
export const createWebhook = (data: Access.WebhookSaveDto) =>
    request.post<void>(WEBHOOKS, webhookSaveSchema.parse(data));
export const updateWebhook = (id: string, data: Access.WebhookSaveDto) =>
    request.put<void>(`${WEBHOOKS}/${accessIdSchema.parse(id)}`, webhookSaveSchema.parse(data));
export const deleteWebhook = (id: string) =>
    request.delete<void>(`${WEBHOOKS}/${accessIdSchema.parse(id)}`);

export const getLogs = (query: Access.LogQuery) =>
    createSnapshotStream<unknown>(appendQueryParams(ACCESS_LOGS, logQuerySchema.parse(query))).map(
        (value) => logPageSchema.parse(value)
    );
