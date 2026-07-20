import { z } from 'zod';
import { pageParamsSchema } from '@/utils/types';

const statusSchema = z.enum(['enabled', 'disabled'], { error: '状态无效' });

export const createDeptSchema = z.object({
    name: z
        .string()
        .min(2, '部门名称长度需在 2 - 128 之间')
        .max(128, '部门名称长度需在 2 - 128 之间'),
    code: z.string().max(64, '部门编码不能超过 64 个字符').optional(),
    parent_id: z.number().int().min(0, '上级部门无效').optional(),
    leader_id: z.number().int().min(0, '负责人无效').optional(),
    sort_order: z.number().int().min(0, '排序不能小于 0').optional(),
    status: statusSchema.optional(),
});

export const updateDeptSchema = createDeptSchema.partial();
export const deptListQuerySchema = pageParamsSchema.extend({
    status: statusSchema.optional(),
    parent_id: z.coerce.number().int().min(0, '上级部门无效').optional(),
});
export const deptIdSchema = z.number().int().min(1, 'id 必须是正整数');
