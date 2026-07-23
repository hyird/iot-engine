import { z } from 'zod';

const statusSchema = z.enum(['enabled', 'disabled']);
const timezoneSchema = z.string().regex(/^[+-](0\d|1[0-4]):[0-5]\d$/, '设备时区格式必须为 ±HH:MM');
const packetSchema = z
    .object({
        mode: z.enum(['OFF', 'HEX', 'ASCII']),
        content: z.string().optional(),
    })
    .superRefine(({ mode, content }, context) => {
        if (mode === 'OFF') return;
        if (!content?.length) {
            context.addIssue({
                code: 'custom',
                path: ['content'],
                message: '报文内容不能为空',
            });
            return;
        }
        if (mode === 'ASCII' && content.length > 256) {
            context.addIssue({
                code: 'custom',
                path: ['content'],
                message: 'ASCII 报文不能超过 256 字节',
            });
            return;
        }
        if (mode !== 'HEX') return;
        const normalized = content.replace(/\s/g, '');
        if (!/^(?:[0-9A-Fa-f]{2})+$/.test(normalized)) {
            context.addIssue({
                code: 'custom',
                path: ['content'],
                message: 'HEX 内容必须由完整字节组成',
            });
        } else if (normalized.length / 2 > 256) {
            context.addIssue({
                code: 'custom',
                path: ['content'],
                message: 'HEX 报文不能超过 256 字节',
            });
        }
    });

const ipv4Schema = z
    .string()
    .regex(
        /^(?:0|[1-9]\d?|1\d\d|2[0-4]\d|25[0-5])(?:\.(?:0|[1-9]\d?|1\d\d|2[0-4]\d|25[0-5])){3}$/,
        '请输入合法 IPv4 地址'
    );

export const saveDeviceSchema = z
    .object({
        name: z.string().trim().min(1, '设备名称不能为空').max(100, '设备名称最多100个字符'),
        device_code: z
            .string()
            .trim()
            .min(1, '设备编码不能为空')
            .max(100, '设备编码最多100个字符')
            .regex(/^[A-Za-z0-9]+$/, '设备编码只能包含字母和数字'),
        link_id: z.uuid({ error: '请选择有效链路' }).optional(),
        edge_node_id: z.uuid({ error: '请选择有效边缘节点' }).optional(),
        edge_transport: z.enum(['serial', 'tcp']).optional(),
        edge_interface: z.string().trim().min(1).max(96).optional(),
        edge_mode: z.enum(['TCP Client', 'TCP Server']).optional(),
        edge_ip: ipv4Schema.optional(),
        edge_port: z.coerce.number().int().min(1).max(65535).optional(),
        serial_baud_rate: z.coerce.number().int().min(300).max(4_000_000).optional(),
        serial_data_bits: z.coerce.number().int().min(5).max(8).optional(),
        serial_stop_bits: z.coerce.number().int().min(1).max(2).optional(),
        serial_parity: z.enum(['none', 'even', 'odd']).optional(),
        serial_rs485: z.boolean().optional(),
        target_id: z.string().trim().max(100).optional(),
        protocol_config_id: z.uuid({ error: '请选择有效设备类型' }),
        group_id: z.uuid({ error: '请选择有效设备分组' }).nullish(),
        status: statusSchema.default('enabled'),
        online_timeout: z.coerce.number().int().min(1).max(86400).default(300),
        remote_control: z.boolean().default(true),
        modbus_mode: z.enum(['TCP', 'RTU']).nullish(),
        slave_id: z.coerce.number().int().min(1).max(247).nullish(),
        timezone: timezoneSchema.default('+08:00'),
        heartbeat: packetSchema.default({ mode: 'OFF' }),
        registration: packetSchema.default({ mode: 'OFF' }),
        remark: z.string().optional(),
    })
    .superRefine((value, context) => {
        if (Boolean(value.link_id) === Boolean(value.edge_node_id)) {
            context.addIssue({
                code: 'custom',
                path: ['link_id'],
                message: '本地链路和边缘节点必须且只能选择一个',
            });
        }
        if (!value.edge_node_id) return;
        if (!value.edge_transport) {
            context.addIssue({
                code: 'custom',
                path: ['edge_transport'],
                message: '请选择传输方式',
            });
        }
        if (!value.edge_interface) {
            context.addIssue({
                code: 'custom',
                path: ['edge_interface'],
                message: '请选择节点接口',
            });
        }
        if (value.edge_transport === 'tcp') {
            if (!value.edge_mode)
                context.addIssue({
                    code: 'custom',
                    path: ['edge_mode'],
                    message: '请选择 TCP 模式',
                });
            if (!value.edge_ip)
                context.addIssue({
                    code: 'custom',
                    path: ['edge_ip'],
                    message: '请输入 IPv4 地址',
                });
            if (!value.edge_port)
                context.addIssue({ code: 'custom', path: ['edge_port'], message: '请输入端口' });
        }
    });

export const saveDeviceGroupSchema = z.object({
    name: z.string().trim().min(1, '分组名称不能为空').max(100, '分组名称最多100个字符'),
    parent_id: z.uuid({ error: '请选择有效上级分组' }).nullish(),
    status: statusSchema.default('enabled'),
    sort_order: z.coerce.number().int().min(0).default(0),
    remark: z.string().optional(),
});

export const deviceIdSchema = z.uuid({ error: 'id 必须是 UUID' });

const commandElementSchema = z.object({
    elementId: z.uuid({ error: '下发要素 ID 必须是 UUID' }),
    value: z.string().trim().min(1, '下发要素值不能为空').max(4096, '下发要素值最多 4096 个字符'),
});

export const deviceCommandSchema = z
    .object({
        elements: z
            .array(commandElementSchema)
            .min(1, '请至少选择一个下发要素')
            .max(256, '单次最多下发 256 个要素'),
    })
    .superRefine(({ elements }, context) => {
        const ids = new Set<string>();
        elements.forEach((element, index) => {
            if (!ids.has(element.elementId)) {
                ids.add(element.elementId);
                return;
            }
            context.addIssue({
                code: 'custom',
                path: ['elements', index, 'elementId'],
                message: '一次下发不能包含重复要素',
            });
        });
    });

const deviceShareSchema = z.object({
    subject_type: z.enum(['user', 'department']),
    subject_id: z.uuid({ error: '分享对象 ID 必须是 UUID' }),
    access_level: z.enum(['view', 'operate']),
});

export const replaceDeviceSharesSchema = z
    .object({
        shares: z.array(deviceShareSchema).max(500, '单次最多设置 500 个分享对象'),
    })
    .superRefine(({ shares }, context) => {
        const uniqueSubjects = new Set<string>();
        shares.forEach((share, index) => {
            const key = `${share.subject_type}:${share.subject_id}`;
            if (!uniqueSubjects.has(key)) {
                uniqueSubjects.add(key);
                return;
            }
            context.addIssue({
                code: 'custom',
                path: ['shares', index, 'subject_id'],
                message: '分享对象不能重复',
            });
        });
    });
