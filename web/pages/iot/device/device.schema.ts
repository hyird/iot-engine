import { z } from 'zod';

const statusSchema = z.enum(['enabled', 'disabled']);
const timezoneSchema = z.string().regex(/^[+-](0\d|1[0-4]):[0-5]\d$/, '设备时区格式必须为 ±HH:MM');
const packetSchema = z.object({
    mode: z.enum(['OFF', 'HEX', 'ASCII']),
    content: z.string().optional(),
});

export const saveDeviceSchema = z.object({
    name: z.string().trim().min(1, '设备名称不能为空').max(100, '设备名称最多100个字符'),
    device_code: z
        .string()
        .trim()
        .min(1, '设备编码不能为空')
        .max(100, '设备编码最多100个字符')
        .regex(/^[A-Za-z0-9]+$/, '设备编码只能包含字母和数字'),
    link_id: z.coerce.number().int().positive('请选择链路'),
    target_id: z.string().trim().max(100).optional(),
    protocol_config_id: z.coerce.number().int().positive('请选择设备类型'),
    group_id: z.coerce.number().int().positive().nullish(),
    status: statusSchema.default('enabled'),
    online_timeout: z.coerce.number().int().min(1).max(86400).default(300),
    remote_control: z.boolean().default(true),
    modbus_mode: z.enum(['TCP', 'RTU']).nullish(),
    slave_id: z.coerce.number().int().min(1).max(247).nullish(),
    timezone: timezoneSchema.default('+08:00'),
    heartbeat: packetSchema.default({ mode: 'OFF' }),
    registration: packetSchema.default({ mode: 'OFF' }),
    remark: z.string().optional(),
});

export const saveDeviceGroupSchema = z.object({
    name: z.string().trim().min(1, '分组名称不能为空').max(100, '分组名称最多100个字符'),
    parent_id: z.coerce.number().int().positive().nullish(),
    status: statusSchema.default('enabled'),
    sort_order: z.coerce.number().int().min(0).default(0),
    remark: z.string().optional(),
});

export const deviceIdSchema = z.number().int().positive('id 必须是正整数');
