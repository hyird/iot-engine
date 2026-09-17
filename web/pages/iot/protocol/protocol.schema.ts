import { z } from 'zod';
export const protocolIdSchema = z.uuid({ error: 'id 必须是 UUID' });
export const protocolTypeSchema = z.enum(['SL651', 'Modbus', 'S7', 'MC', 'FINS', 'DLT645']);

export function industrialConfigSchema(protocol: 'MC' | 'FINS' | 'DLT645') {
    const integer = (max: number, min = 0) => z.number().int().min(min).max(max).optional();
    const connection =
        protocol === 'MC'
            ? z.object({
                  frame: z.enum(['3E', '4E']).optional(),
                  network: integer(255),
                  station: integer(255),
                  moduleIo: integer(65535),
                  multidrop: integer(255),
                  monitoringTimer: integer(65535, 1),
              })
            : protocol === 'FINS'
              ? z.object({
                    sourceNetwork: integer(127),
                    sourceNode: integer(254),
                    sourceUnit: integer(255),
                    destinationNetwork: integer(127),
                    destinationNode: integer(254),
                    destinationUnit: integer(255),
                })
              : z.object({
                    version: z.enum(['1997', '2007']).optional(),
                    wakeupBytes: integer(4),
                    writePassword: z
                        .string()
                        .regex(/^(?:[\da-fA-F]{8})?$/)
                        .optional(),
                    operatorCode: z
                        .string()
                        .regex(/^(?:[\da-fA-F]{8})?$/)
                        .optional(),
                });
    return z
        .object({
            storagePolicy: z.enum(['report', 'change']),
            readInterval: integer(3600, 1),
            commandFastReadDuration: integer(3600),
            commandFastReadInterval: integer(3600, 1),
            connection,
            points: z
                .array(
                    z.object({
                        id: z.uuid(),
                        name: z.string().trim().min(1).max(100),
                        unit: z.string().max(32).optional(),
                        writable: z.boolean().optional(),
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
                            'BCD',
                            'BCD_SIGNED',
                            'HEX',
                        ]),
                        area: z.string().optional(),
                        address: integer(protocol === 'MC' ? 16777215 : 65535),
                        bit: integer(15),
                        byteOrder: z
                            .enum([
                                'BIG_ENDIAN',
                                'LITTLE_ENDIAN',
                                'BIG_ENDIAN_BYTE_SWAP',
                                'LITTLE_ENDIAN_BYTE_SWAP',
                            ])
                            .optional(),
                        scale: z.number().finite().optional(),
                        decimals: integer(8, -1),
                        identifier: z.string().optional(),
                        length: integer(200, 1),
                        digits: integer(8),
                    })
                )
                .max(256),
        })
        .superRefine((config, context) => {
            const ids = new Set<string>();
            for (const [index, point] of config.points.entries()) {
                const invalid = (message: string) =>
                    context.addIssue({ code: 'custom', path: ['points', index], message });
                if (ids.has(point.id)) invalid('点位标识不能重复');
                ids.add(point.id);
                if (protocol === 'DLT645') {
                    const meter = config.connection as {
                        version?: string;
                        writePassword?: string;
                        operatorCode?: string;
                    };
                    const legacy = meter.version === '1997';
                    if (
                        !new RegExp(`^[0-9A-Fa-f]{${legacy ? 4 : 8}}$`).test(point.identifier ?? '')
                    )
                        invalid('数据标识与 DL/T645 版本不匹配');
                    if (
                        !['BCD', 'BCD_SIGNED', 'HEX'].includes(point.dataType) ||
                        !point.length ||
                        (point.dataType !== 'HEX' && point.length > 8)
                    )
                        invalid('电表数据类型或长度无效');
                    if (
                        point.writable &&
                        (!meter.writePassword ||
                            (!legacy && !meter.operatorCode) ||
                            (point.length ?? 0) > (legacy ? 44 : 38))
                    )
                        invalid('可写点位必须配置认证字段，长度不得超过写帧上限');
                    continue;
                }
                const bits = point.dataType === 'BOOL';
                if (['BCD', 'BCD_SIGNED', 'HEX'].includes(point.dataType))
                    invalid('PLC 数据类型无效');
                const width =
                    bits || ['INT16', 'UINT16'].includes(point.dataType)
                        ? 1
                        : ['INT64', 'UINT64', 'DOUBLE'].includes(point.dataType)
                          ? 4
                          : 2;
                if (point.address === undefined) invalid('请输入十进制地址');
                if (protocol === 'MC') {
                    const bitArea = ['M', 'X', 'Y', 'B', 'L', 'F', 'V', 'S', 'TS', 'CS'].includes(
                        point.area ?? ''
                    );
                    if (
                        (!bitArea &&
                            !['D', 'W', 'R', 'ZR', 'TN', 'CN'].includes(point.area ?? '')) ||
                        (bits && !bitArea)
                    )
                        invalid('软元件区域与数据类型不匹配');
                    if ((point.address ?? 0) + width * (bitArea && !bits ? 16 : 1) - 1 > 16777215)
                        invalid('地址范围越界');
                } else if (
                    !['D', 'CIO', 'W', 'H', 'A'].includes(point.area ?? '') ||
                    (!bits && (point.bit ?? 0) !== 0) ||
                    (point.address ?? 0) + (bits ? 0 : width - 1) > 65535
                )
                    invalid('FINS 区域、位地址或范围无效');
            }
        });
}
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
        if (value.protocol === 'MC' || value.protocol === 'FINS' || value.protocol === 'DLT645') {
            const result = industrialConfigSchema(value.protocol).safeParse(config);
            if (!result.success)
                for (const issue of result.error.issues)
                    context.addIssue({
                        code: 'custom',
                        path: ['config', ...issue.path],
                        message: issue.message,
                    });
            return;
        }
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

const normalizeTsapValue = (value?: string) => {
    if (!value) return undefined;
    const normalized = value
        .replace(/^0x/i, '')
        .replace(/[\s.:\-_]/g, '')
        .toUpperCase();
    return normalized || undefined;
};
export const formatTsapValue = (value?: string) => {
    const normalized = normalizeTsapValue(value);
    if (!normalized || !/^[0-9A-F]{1,4}$/.test(normalized)) {
        return undefined;
    }
    return normalized.padStart(4, '0');
};
export const validateTsapValue = async (_: unknown, value?: string) => {
    if (!formatTsapValue(value)) {
        throw new Error('请输入 1-4 位十六进制 TSAP，例如 4D57 或 0200');
    }
};
