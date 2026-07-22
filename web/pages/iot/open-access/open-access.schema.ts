import { z } from 'zod';
import { pageParamsSchema } from '@/utils/types';

export const openStatusSchema = z.enum(['enabled', 'disabled']);
export const openScopeSchema = z.enum([
    'device:realtime',
    'device:history',
    'device:command',
    'alert:read',
]);
export const webhookEventSchema = z.enum([
    'device.data.reported',
    'device.image.reported',
    'device.command.dispatched',
    'device.command.responded',
    'device.alert.triggered',
    'device.alert.resolved',
]);
const uuidSchema = z.uuid({ error: '必须是 UUID' });
export const openAccessIdSchema = uuidSchema;
const nullableString = z.string().nullable();

export const keySaveSchema = z.object({
    name: z.string().trim().min(1, '请输入调用配置名称').max(64),
    status: openStatusSchema,
    scopes: z.array(openScopeSchema).min(1, '至少选择一个开放权限'),
    deviceIds: z.array(uuidSchema).min(1, '至少选择一台设备').max(10000),
    expiresAt: z
        .union([z.literal(''), z.iso.datetime({ offset: true }), z.null()])
        .optional()
        .transform((value) => value || null),
    remark: z.string().trim().max(200).nullable().optional(),
});

export const webhookSaveSchema = z.object({
    accessKeyId: uuidSchema,
    name: z.string().trim().min(1, '请输入 Webhook 名称').max(64),
    url: z.url({ protocol: /^https?$/ }).max(2048),
    status: openStatusSchema,
    timeoutSeconds: z.coerce.number().int().min(1).max(30),
    headers: z.record(z.string(), z.string()),
    eventTypes: z.array(webhookEventSchema).min(1, '至少选择一个事件'),
    secret: z.string().trim().max(255).nullable().optional(),
});

export const webhookFormSchema = webhookSaveSchema
    .omit({ headers: true })
    .extend({ headersText: z.string().max(10000, 'Header JSON 过长') })
    .transform(({ headersText, ...data }, context) => {
        try {
            const headers = z.record(z.string(), z.string()).parse(JSON.parse(headersText || '{}'));
            return { ...data, headers };
        } catch {
            context.addIssue({
                code: 'custom',
                path: ['headersText'],
                message: '请输入值均为字符串的 JSON 对象',
            });
            return z.NEVER;
        }
    });

export const logQuerySchema = pageParamsSchema.extend({
    accessKeyId: uuidSchema.optional(),
    webhookId: uuidSchema.optional(),
    direction: z.enum(['pull', 'push']).optional(),
    status: z.enum(['success', 'failed']).optional(),
    action: z.string().max(50).optional(),
});

export const deviceOptionSchema = z.object({
    id: uuidSchema,
    name: z.string(),
    deviceCode: z.string(),
    canCommand: z.boolean(),
});

export const keyItemSchema = z.object({
    id: uuidSchema,
    name: z.string(),
    accessKeyPrefix: z.string(),
    status: openStatusSchema,
    scopes: z.array(openScopeSchema),
    deviceIds: z.array(uuidSchema),
    expiresAt: nullableString,
    lastUsedAt: nullableString,
    lastUsedIp: nullableString,
    remark: nullableString,
    webhookCount: z.number().int(),
    createdAt: z.string(),
    updatedAt: z.string(),
});

export const keySecretSchema = z.object({
    id: uuidSchema,
    name: z.string(),
    accessKey: z.string(),
    accessKeyPrefix: z.string(),
});

export const webhookItemSchema = z.object({
    id: uuidSchema,
    accessKeyId: uuidSchema,
    accessKeyName: z.string(),
    name: z.string(),
    url: z.string(),
    status: openStatusSchema,
    timeoutSeconds: z.number().int(),
    headers: z.record(z.string(), z.string()),
    eventTypes: z.array(webhookEventSchema),
    deviceIds: z.array(uuidSchema),
    hasSecret: z.boolean(),
    lastTriggeredAt: nullableString,
    lastSuccessAt: nullableString,
    lastFailureAt: nullableString,
    lastHttpStatus: z.number().int().nullable(),
    lastError: nullableString,
    createdAt: z.string(),
    updatedAt: z.string(),
});

export const logItemSchema = z.object({
    id: uuidSchema,
    accessKeyId: uuidSchema.nullable(),
    accessKeyName: nullableString,
    webhookId: uuidSchema.nullable(),
    webhookName: nullableString,
    direction: z.enum(['pull', 'push']),
    action: z.string(),
    eventType: nullableString,
    status: z.enum(['success', 'failed']),
    httpMethod: nullableString,
    target: nullableString,
    requestIp: nullableString,
    httpStatus: z.number().int().nullable(),
    deviceId: uuidSchema.nullable(),
    deviceCode: nullableString,
    message: nullableString,
    requestPayload: z.record(z.string(), z.unknown()),
    responsePayload: z.record(z.string(), z.unknown()),
    createdAt: z.string(),
});

export const logPageSchema = z.object({
    list: z.array(logItemSchema),
    total: z.number().int(),
    page: z.number().int(),
    pageSize: z.number().int(),
    totalPages: z.number().int(),
});
