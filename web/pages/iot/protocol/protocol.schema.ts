import { z } from 'zod';

export const protocolIdSchema = z.number().int().positive('id 必须是正整数');
export const protocolTypeSchema = z.enum(['SL651', 'Modbus', 'S7']);

const configSchema = z.record(z.string(), z.unknown());
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
            if (!Array.isArray(config.registers))
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'registers'],
                    message: '寄存器必须是数组',
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
            if (!Array.isArray(config.areas))
                context.addIssue({
                    code: 'custom',
                    path: ['config', 'areas'],
                    message: '寄存器必须是数组',
                });
        }
    });

export const protocolUpdateSchema = baseSchema.partial();
