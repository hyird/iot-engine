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

const ipv4TextSchema = z
    .string()
    .refine((value) => parseIpv4(value) !== null, '请输入有效的 IPv4 地址');

const networkDeviceSchema = z
    .string()
    .min(1, '请选择网卡')
    .max(32)
    .regex(/^[A-Za-z0-9_.:-]+$/, '网卡名称包含非法字符');

const logicalInterfaceNameSchema = z
    .string()
    .min(1, '逻辑接口名称不能为空')
    .max(15, '逻辑接口名称不能超过 15 个字符')
    .regex(/^[A-Za-z0-9_]+$/, '逻辑接口名称只能包含字母、数字和下划线')
    .refine((value) => value !== 'loopback', 'loopback 接口不允许远程修改');

export const networkInterfaceSchema = z
    .object({
        operation: z.enum(['upsert', 'delete']),
        name: logicalInterfaceNameSchema,
        previousName: logicalInterfaceNameSchema.optional(),
        mode: z.enum(['dhcp', 'static']).optional(),
        device: z.union([z.literal(''), networkDeviceSchema]).optional(),
        bridge: z.boolean().optional(),
        bridgePorts: z.array(networkDeviceSchema).max(8).optional(),
        ip: z.union([z.literal(''), ipv4TextSchema]).optional(),
        prefixLength: z.number().int().min(0).max(30).optional(),
        gateway: z.union([z.literal(''), ipv4TextSchema]).optional(),
    })
    .superRefine((value, context) => {
        if (value.previousName) {
            if (value.operation !== 'upsert' || value.previousName === value.name) {
                context.addIssue({
                    code: 'custom',
                    path: ['previousName'],
                    message: '原逻辑接口名称无效',
                });
            }
        }
        if (value.operation === 'delete') return;
        if (!value.mode) {
            context.addIssue({
                code: 'custom',
                path: ['mode'],
                message: '请选择 DHCP 或静态 IPv4',
            });
        }
        if (value.bridge) {
            if (value.name.length > 12) {
                context.addIssue({
                    code: 'custom',
                    path: ['name'],
                    message: '网桥逻辑名称不能超过 12 个字符',
                });
            }
            if (!value.bridgePorts?.length) {
                context.addIssue({
                    code: 'custom',
                    path: ['bridgePorts'],
                    message: '请至少选择一个网桥成员',
                });
            }
        } else if (!value.device) {
            context.addIssue({
                code: 'custom',
                path: ['device'],
                message: '请选择一个网卡',
            });
        }
        if (value.mode === 'dhcp') {
            if (value.ip || value.gateway || (value.prefixLength ?? 0) !== 0) {
                context.addIssue({
                    code: 'custom',
                    path: ['ip'],
                    message: 'DHCP 接口不能填写静态 IPv4',
                });
            }
            return;
        }
        if (value.mode !== 'static') return;
        const address = value.ip ? parseIpv4(value.ip) : null;
        const prefix = value.prefixLength ?? 0;
        if (address === null) {
            context.addIssue({ code: 'custom', path: ['ip'], message: '请输入有效的 IPv4 地址' });
            return;
        }
        if (prefix < 1 || prefix > 30) {
            context.addIssue({
                code: 'custom',
                path: ['prefixLength'],
                message: 'IPv4 前缀必须在 1 - 30 之间',
            });
            return;
        }
        const mask = (0xffffffff << (32 - prefix)) >>> 0;
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

export const networkSchema = z
    .object({
        interfaces: z.array(networkInterfaceSchema).min(1).max(8),
        rollbackTimeoutSec: z.number().int().min(30).max(300),
    })
    .superRefine((value, context) => {
        const names = new Set<string>();
        const previousNames = new Set<string>();
        const devices = new Set<string>();
        value.interfaces.forEach((item, index) => {
            if (names.has(item.name)) {
                context.addIssue({
                    code: 'custom',
                    path: ['interfaces', index, 'name'],
                    message: '同一请求不能重复配置逻辑接口',
                });
            }
            names.add(item.name);
            if (item.previousName) {
                if (previousNames.has(item.previousName)) {
                    context.addIssue({
                        code: 'custom',
                        path: ['interfaces', index, 'previousName'],
                        message: '同一请求不能重复修改原逻辑接口',
                    });
                }
                previousNames.add(item.previousName);
            }
            if (item.operation === 'delete') return;
            const selected = item.bridge
                ? (item.bridgePorts ?? [])
                : item.device
                  ? [item.device]
                  : [];
            selected.forEach((device) => {
                if (devices.has(device)) {
                    context.addIssue({
                        code: 'custom',
                        path: ['interfaces', index, item.bridge ? 'bridgePorts' : 'device'],
                        message: '同一网卡不能重复分配',
                    });
                }
                devices.add(device);
            });
        });
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

export const modemControlSchema = z
    .object({
        action: z.enum(['set_apn', 'redial']),
        apn: z
            .string()
            .max(63, 'APN 不能超过 63 个字符')
            .regex(/^[A-Za-z0-9._-]*$/, 'APN 只能包含字母、数字、点、下划线和连字符'),
    })
    .superRefine((value, context) => {
        if (value.action === 'set_apn' && value.apn.length === 0) {
            context.addIssue({ code: 'custom', path: ['apn'], message: 'APN 不能为空' });
        }
    });

export const firmwareUpgradeSchema = z.object({
    file: z
        .instanceof(File)
        .refine(
            (file) => file.size > 0 && file.size <= 128 * 1024 * 1024,
            '固件必须在 1 B 到 128 MiB 之间'
        ),
    keepSettings: z.boolean(),
});

export const nodeNameSchema = z.object({
    name: z.string().min(1, '节点名称不能为空').max(100, '节点名称不能超过 100 个字符'),
});

export const edgeListQuerySchema = pageParamsSchema.extend({
    status: enrollmentStatusSchema.optional(),
    keyword: z.string().optional(),
});
export const edgeIdSchema = z.uuid('节点 ID 必须是 UUID');
export const platformIdSchema = z.uuid('平台 ID 必须是 UUID');
