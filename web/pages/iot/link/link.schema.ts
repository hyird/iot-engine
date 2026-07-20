import { z } from 'zod';
import { pageParamsSchema } from '@/utils/types';

const ipv4Schema = z.ipv4({ error: '请输入有效的 IPv4 地址' });
const modeSchema = z.enum(['TCP Server', 'TCP Client'], { error: '链路模式无效' });
const protocolSchema = z.enum(['SL651', 'Modbus', 'S7'], { error: '协议无效' });
const statusSchema = z.enum(['enabled', 'disabled'], { error: '状态无效' });

const targetSchema = z.object({
    id: z.string().min(1, '目标 ID 不能为空').max(64, '目标 ID 不能超过 64 个字符'),
    name: z.string().min(1, '目标名称不能为空').max(100, '目标名称不能超过 100 个字符'),
    ip: ipv4Schema,
    port: z.number().int().min(1, '端口必须在 1 - 65535 之间').max(65535),
    status: statusSchema,
});

export const saveLinkSchema = z
    .object({
        name: z.string().min(1, '链路名称不能为空').max(100, '链路名称不能超过 100 个字符'),
        mode: modeSchema,
        protocol: protocolSchema,
        ip: z.string(),
        port: z.number().int().min(0).max(65535),
        targets: z.array(targetSchema).max(100, '单条链路最多配置 100 个目标'),
        status: statusSchema,
    })
    .superRefine((value, context) => {
        if (value.mode === 'TCP Server') {
            const result = ipv4Schema.safeParse(value.ip);
            if (!result.success)
                context.addIssue({
                    code: 'custom',
                    path: ['ip'],
                    message: result.error.issues[0].message,
                });
            if (value.port < 1)
                context.addIssue({
                    code: 'custom',
                    path: ['port'],
                    message: '端口必须在 1 - 65535 之间',
                });
            if (value.targets.length)
                context.addIssue({
                    code: 'custom',
                    path: ['targets'],
                    message: 'TCP Server 不能配置目标地址',
                });
        } else if (!value.targets.length) {
            context.addIssue({
                code: 'custom',
                path: ['targets'],
                message: 'TCP Client 至少需要一个目标地址',
            });
        }
    });

export const linkListQuerySchema = pageParamsSchema.extend({
    mode: modeSchema.optional(),
    protocol: protocolSchema.optional(),
    status: statusSchema.optional(),
});
export const linkIdSchema = z.number().int().min(1, 'id 必须是正整数');
