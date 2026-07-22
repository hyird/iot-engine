import { z } from 'zod';
import { pageParamsSchema } from '@/utils/types';

const enrollmentStatusSchema = z.enum(['pending', 'approved', 'rejected']);

function parseIpv4(value: string) {
    const parts = value.split('.');
    if (parts.length !== 4) return null;
    const numbers = parts.map((part) => {
        if (!/^(0|[1-9]\d{0,2})$/.test(part)) return Number.NaN;
        return Number(part);
    });
    if (numbers.some((part) => !Number.isInteger(part) || part < 0 || part > 255)) return null;
    return numbers.reduce((result, part) => (result * 256 + part) >>> 0, 0);
}

function netmaskPrefix(value: string) {
    const mask = parseIpv4(value);
    if (mask === null) return null;
    let zeroSeen = false;
    let prefix = 0;
    for (let bit = 31; bit >= 0; bit -= 1) {
        const one = (mask & (2 ** bit)) !== 0;
        if (zeroSeen && one) return null;
        if (one) prefix += 1;
        else zeroSeen = true;
    }
    return prefix >= 1 && prefix <= 30 ? prefix : null;
}

const ipv4TextSchema = z
    .string()
    .refine((value) => parseIpv4(value) !== null, '请输入有效的 IPv4 地址');

export const networkSchema = z
    .object({
        ip: ipv4TextSchema,
        netmask: z
            .string()
            .refine((value) => netmaskPrefix(value) !== null, '请输入连续的 /1 到 /30 子网掩码'),
        gateway: z.union([z.literal(''), ipv4TextSchema]).optional(),
        rollbackTimeoutSec: z.number().int().min(30).max(300),
    })
    .superRefine((value, context) => {
        const address = parseIpv4(value.ip);
        const mask = parseIpv4(value.netmask);
        if (address === null || mask === null || netmaskPrefix(value.netmask) === null) return;
        const hostMask = ~mask >>> 0;
        const host = address & hostMask;
        if (host === 0 || host === hostMask) {
            context.addIssue({
                code: 'custom',
                path: ['ip'],
                message: 'IP 不能是网络地址或广播地址',
            });
        }
        if (value.gateway) {
            const gateway = parseIpv4(value.gateway);
            if (
                gateway === null ||
                (gateway & mask) !== (address & mask) ||
                gateway === address ||
                (gateway & hostMask) === 0 ||
                (gateway & hostMask) === hostMask
            ) {
                context.addIssue({
                    code: 'custom',
                    path: ['gateway'],
                    message: '网关必须是同网段内不同的合法主机地址',
                });
            }
        }
    });

export const platformSchema = z.object({
    platformId: z.uuid('平台 ID 必须是 UUID').optional(),
    name: z.string().min(1, '平台名称不能为空').max(32),
    baseUrl: z
        .url('请输入合法平台地址')
        .max(255)
        .refine(
            (value) => value.startsWith('http://') || value.startsWith('https://'),
            '平台地址只支持 HTTP 或 HTTPS'
        ),
    enrollmentToken: z.string().max(192).optional(),
    enabled: z.boolean(),
    priority: z.number().int().min(0).max(65535),
    reconnectIntervalSec: z.number().int().min(1).max(3600),
    outboxMaxBytes: z.number().int().min(16384).max(8388608),
});

export const firmwareUploadSchema = z.object({
    version: z.string().min(1, '固件版本不能为空').max(64),
    file: z
        .instanceof(File)
        .refine(
            (file) => file.size > 0 && file.size <= 128 * 1024 * 1024,
            '固件必须在 1 B 到 128 MiB 之间'
        ),
});

export const firmwareTaskSchema = z.object({
    firmwareId: z.uuid('请选择固件'),
    keepSettings: z.boolean(),
});

export const edgeListQuerySchema = pageParamsSchema.extend({
    status: enrollmentStatusSchema.optional(),
    keyword: z.string().optional(),
});
export const edgeIdSchema = z.uuid('节点 ID 必须是 UUID');
export const platformIdSchema = z.uuid('平台 ID 必须是 UUID');
