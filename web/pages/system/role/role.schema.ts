import { z } from 'zod';
import { pageParamsSchema } from '@/utils/types';

const statusSchema = z.enum(['enabled', 'disabled'], { error: '状态无效' });

export const createRoleSchema = z.object({
    name: z
        .string()
        .min(2, '角色名称长度需在 2 - 128 之间')
        .max(128, '角色名称长度需在 2 - 128 之间'),
    code: z.string().min(2, '角色编码长度需在 2 - 64 之间').max(64, '角色编码长度需在 2 - 64 之间'),
    description: z.string().max(500, '角色描述不能超过 500 个字符').optional(),
    status: statusSchema.optional(),
    permissions: z.array(z.string()).optional(),
});

export const updateRoleSchema = createRoleSchema.partial();
export const roleListQuerySchema = pageParamsSchema.extend({
    status: statusSchema.optional(),
});
export const roleIdSchema = z.uuid({ error: 'id 必须是 UUID' });
