import { z } from 'zod';

export const protocolIdSchema = z.uuid({ error: 'id 必须是 UUID' });
export const protocolTypeSchema = z.enum(['SL651', 'Modbus', 'S7']);

const configSchema = z.record(z.string(), z.unknown());
const modbusRegisterSchema = z.object({
    id: z.string().min(1, '寄存器 ID 不能为空'),
    name: z.string().min(1, '寄存器名称不能为空'),
    registerType: z.enum(['COIL', 'DISCRETE_INPUT', 'HOLDING_REGISTER', 'INPUT_REGISTER']),
    dataType: z.enum([
        'BOOL',
        'INT16',
        'UINT16',
        'INT32',
        'UINT32',
        'FLOAT32',
        'INT64',
        'UINT64',
        'DOUBLE',
    ]),
    address: z
        .number()
        .int('寄存器地址必须是整数')
        .min(0, '寄存器地址不能小于 0')
        .max(65535, '寄存器地址不能大于 65535'),
    quantity: z
        .number()
        .int('寄存器数量必须是整数')
        .min(1, '寄存器数量不能小于 1')
        .max(4, '寄存器数量不能大于 4'),
});

const s7AreaSchema = z
    .object({
        id: z.string().min(1, '寄存器 ID 不能为空'),
        name: z.string().min(1, '寄存器名称不能为空'),
        group: z.string().optional(),
        area: z.enum(['DB', 'V', 'MK', 'PE', 'PA', 'CT', 'TM']),
        dataType: z
            .enum(['BOOL', 'INT8', 'UINT8', 'INT16', 'UINT16', 'INT32', 'UINT32', 'FLOAT', 'LREAL', 'STRING'])
            .optional(),
        dbNumber: z.number().int().min(1, 'DB 编号不能小于 1').optional(),
        start: z.number().int().min(0, '起始偏移不能小于 0'),
        startBit: z.number().int().min(0, '位号不能小于 0').max(7, '位号只能是 0~7').optional(),
        size: z.number().int().min(1, '长度不能小于 1'),
        unit: z.string().optional(),
        decimals: z.number().int().min(-1, '小数位不能小于 -1').max(8, '小数位不能大于 8').optional(),
        writable: z.boolean().optional(),
        remark: z.string().optional(),
    })
    .superRefine((area, context) => {
        if (area.area === 'DB' && area.dbNumber === undefined)
            context.addIssue({
                code: 'custom',
                path: ['dbNumber'],
                message: 'DB 区域必须填写 DB 编号',
            });
    });

function isObject(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

const baseSchema = z.object({
    name: z.string().trim().min(1, '请输入配置名称').max(64, '配置名称最多64个字符'),
    enabled: z.boolean().optional(),
    config: configSchema,
    remark: z.string().optional(),
});

export const protocolCreateSchema = baseSchema
    .extend({ protocol: protocolTypeSchema })
    .superRefine((value, context) => {
        const config = value.config;
        if (value.protocol === 'Modbus') {
            if (
                ![
                    'BIG_ENDIAN',
                    'LITTLE_ENDIAN',
                    'BIG_ENDIAN_BYTE_SWAP',
                    'LITTLE_ENDIAN_BYTE_SWAP',
                ].includes(String(config.byteOrder ?? ''))
            )
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'byteOrder'],
                    message: '字节序无效',
                });
            const registersResult = z.array(modbusRegisterSchema).safeParse(config.registers);
            if (!registersResult.success)
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'registers'],
                    message: registersResult.error.issues[0]?.message ?? '寄存器配置无效',
                });
            if (config.packet !== undefined && !isObject(config.packet))
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'packet'],
                    message: '组包配置必须是对象',
                });
        } else if (value.protocol === 'SL651') {
            if (!['M1', 'M2', 'M3', 'M4'].includes(String(config.responseMode ?? '')))
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'responseMode'],
                    message: '应答模式无效',
                });
            if (!Array.isArray(config.funcs))
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'funcs'],
                    message: '功能码必须是数组',
                });
        } else {
            if (
                !['S7-200', 'S7-300', 'S7-400', 'S7-1200', 'S7-1500'].includes(
                    String(config.plcModel ?? '')
                )
            )
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'plcModel'],
                    message: 'PLC型号无效',
                });
            if (!Array.isArray(config.areas)) {
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'areas'],
                    message: '寄存器必须是数组',
                });
            } else {
                const areasResult = z.array(s7AreaSchema).safeParse(config.areas);
                if (!areasResult.success)
                    context.addIssue({
                        code: 'custom',
                        path: ['config', 'areas'],
                        message: areasResult.error.issues[0]?.message ?? 'S7 寄存器配置无效',
                    });
            }
            if (!isObject(config.connection)) {
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'connection'],
                    message: '连接配置必须是对象',
                });
            } else {
                const connection = config.connection;
                if (
                    !['STANDARD', 'COMPATIBLE', 'AUTO'].includes(
                        String(connection.probeMode ?? 'STANDARD')
                    )
                )
                    context.addIssue({
                        code: 'custom',
                        path: ['config', 'connection', 'probeMode'],
                        message: '连接探测模式无效',
                    });
                for (const [field, label] of [
                    ['handshakeTimeout', '握手超时'],
                    ['directProbeTimeout', '兼容探测超时'],
                ] as const) {
                    const value = connection[field] ?? 5000;
                    if (!Number.isInteger(value) || Number(value) < 1000 || Number(value) > 30000)
                        context.addIssue({
                            code: 'custom',
                            path: ['config', 'connection', field],
                            message: `${label}必须在 1000 - 30000 ms 之间`,
                        });
                }
            }
        }
    });

export const protocolUpdateSchema = baseSchema.partial();
