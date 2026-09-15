import { z } from 'zod';
export const protocolIdSchema = z.uuid({ error: 'id 必须是 UUID' });
export const protocolTypeSchema = z.enum(['SL651', 'Modbus', 'S7']);
const sl651ElementSchema = z
    .object({
        guideHex: z.string().optional().default(''),
        positionMode: z.enum(['GUIDE', 'OFFSET']).optional().default('GUIDE'),
        byteOffset: z.number().int().min(0).max(8388607).optional().default(0),
        encode: z.enum(['BCD', 'HEX', 'DICT', 'JPEG', 'TIME_YYMMDDHHMMSS']),
        length: z.number().int().min(0).max(8388608),
        digits: z.number().int().min(0).max(7),
    })
    .superRefine((element, context) => {
        if (element.positionMode === 'OFFSET') {
            if (element.length < 1 || element.byteOffset + element.length > 8388608)
                context.addIssue({
                    code: 'custom',
                    message: '固定位置的长度须大于零，偏移加长度不能超出正文上限',
                });
            return;
        }
        const guide = element.guideHex;
        if (!/^(?:[0-9a-fA-F]{4}|[fF]{2}[0-9a-fA-F]{4})$/.test(guide)) {
            context.addIssue({ code: 'custom', message: '引导符须为两字节，FF 扩展须为三字节' });
            return;
        }
        const lead = Number.parseInt(guide.slice(0, 2), 16);
        const definition = Number.parseInt(guide.slice(-2), 16);
        let valid = (lead === 255) === (guide.length === 6);
        if (lead < 240 || lead === 255) {
            valid &&=
                element.length > 0 &&
                element.length === definition >> 3 &&
                (element.encode !== 'BCD' || element.digits === (definition & 7));
        } else {
            valid &&= definition === lead;
            if (lead === 240) valid &&= element.length === 5;
            else if (lead === 241) valid &&= element.length === 6;
            else if (lead !== 242 && lead !== 243) valid &&= element.length > 0;
        }
        if (!valid)
            context.addIssue({
                code: 'custom',
                message: '引导符、长度或小数位不符合 SL651 数据定义',
            });
    });
const sl651FunctionsSchema = z.array(
    z.object({
        funcCode: z.string().regex(/^[0-9a-fA-F]{2}$/, '功能码须为一个 HEX 字节'),
        dir: z.enum(['UP', 'DOWN']),
        elements: z.array(sl651ElementSchema),
        responseElements: z.array(sl651ElementSchema).optional(),
    })
);
const configSchema = z.record(z.string(), z.unknown()).superRefine((config, context) => {
    if (config.funcs === undefined && config.responseMode === undefined) return;
    if (
        config.responseMode !== undefined &&
        !['M1', 'M2', 'M3', 'M4'].includes(String(config.responseMode))
    )
        context.addIssue({ code: 'custom', path: ['responseMode'], message: '应答模式无效' });
    if (config.funcs === undefined) return;
    const result = sl651FunctionsSchema.safeParse(config.funcs);
    if (!result.success)
        for (const issue of result.error.issues)
            context.addIssue({
                code: 'custom',
                path: ['funcs', ...issue.path],
                message: issue.message,
            });
});
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
            .enum([
                'BOOL',
                'INT8',
                'UINT8',
                'INT16',
                'UINT16',
                'INT32',
                'UINT32',
                'FLOAT',
                'LREAL',
                'STRING',
            ])
            .optional(),
        dbNumber: z.number().int().min(1, 'DB 编号不能小于 1').optional(),
        start: z.number().int().min(0, '起始偏移不能小于 0'),
        startBit: z.number().int().min(0, '位号不能小于 0').max(7, '位号只能是 0~7').optional(),
        size: z.number().int().min(1, '长度不能小于 1'),
        unit: z.string().optional(),
        decimals: z
            .number()
            .int()
            .min(-1, '小数位不能小于 -1')
            .max(8, '小数位不能大于 8')
            .optional(),
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

export function parseProtocolImport(text: string, protocol: z.infer<typeof protocolTypeSchema>) {
    let rawItems: unknown;
    try {
        rawItems = JSON.parse(text);
    } catch {
        throw new Error('JSON 格式错误');
    }
    if (!Array.isArray(rawItems) || rawItems.length === 0) {
        throw new Error('文件内容为空或格式不正确');
    }
    const items: z.infer<typeof protocolCreateSchema>[] = [];
    for (let i = 0; i < rawItems.length; i++) {
        const rawItem = rawItems[i];
        if (typeof rawItem !== 'object' || rawItem === null || Array.isArray(rawItem)) {
            throw new Error(`第 ${i + 1} 项必须是对象`);
        }
        const itemProtocol = (rawItem as Record<string, unknown>).protocol;
        if (itemProtocol !== protocol) {
            const actualProtocol = typeof itemProtocol === 'string' ? itemProtocol : '未指定';
            throw new Error(
                `第 ${i + 1} 项协议类型为 ${actualProtocol}，不能导入到 ${protocol} 页面`
            );
        }
        const parsedItem = protocolCreateSchema.safeParse(rawItem);
        if (!parsedItem.success) {
            const issue = parsedItem.error.issues[0];
            const path = issue.path.length ? `${issue.path.map(String).join('.')}：` : '';
            throw new Error(`第 ${i + 1} 项 ${path}${issue.message}`);
        }
        items.push(parsedItem.data);
    }
    return items;
}
