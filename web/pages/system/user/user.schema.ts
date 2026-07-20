import { z } from 'zod';
import { pageParamsSchema } from '@/utils/types';

export const createUserSchema = z.object({
    username: z.string().min(2, '用户名至少2个字符').max(50, '用户名最多50个字符'),
    password: z.string().min(6, '密码至少6个字符').max(100, '密码最多100个字符'),
    nickname: z.string().max(100, '昵称最多100个字符').optional(),
    phone: z.string().max(20, '手机号最多20位').optional(),
    email: z.string().email('邮箱格式不正确').max(100, '邮箱最多100个字符').optional(),
    status: z.enum(['enabled', 'disabled']).optional(),
    role_ids: z.array(z.number().int().positive()).min(1, '至少选择一个角色'),
});

export const updateUserSchema = z.object({
    nickname: z.string().max(100, '昵称最多100个字符').optional(),
    phone: z.string().max(20, '手机号最多20位').optional(),
    email: z.string().email('邮箱格式不正确').max(100, '邮箱最多100个字符').optional(),
    status: z.enum(['enabled', 'disabled']).optional(),
    password: z.string().min(6, '密码至少6个字符').max(100, '密码最多100个字符').optional(),
    role_ids: z.array(z.number().int().positive()).min(1, '至少选择一个角色').optional(),
});

export const userQuerySchema = pageParamsSchema.extend({
    status: z.enum(['enabled', 'disabled']).optional(),
});

export type CreateUserInput = z.infer<typeof createUserSchema>;
export type UpdateUserInput = z.infer<typeof updateUserSchema>;
export type UserQueryInput = z.infer<typeof userQuerySchema>;
