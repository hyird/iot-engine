import { z } from 'zod';
import { pageParamsSchema } from '@/utils/types';

const statusSchema = z.enum(['enabled', 'disabled'], { error: '状态无效' });
const phoneSchema = z
    .string()
    .refine(
        (value) =>
            value.length === 0 ||
            (value.length >= 7 && value.length <= 20 && /^[0-9+\- ]+$/.test(value)),
        '手机号格式不正确'
    );
const emailSchema = z
    .string()
    .max(100, '邮箱不能超过 100 个字符')
    .refine((value) => {
        if (value.length === 0) return true;
        const at = value.indexOf('@');
        if (at <= 0 || at + 1 >= value.length) return false;
        const dot = value.indexOf('.', at + 1);
        if (dot < 0 || dot + 1 >= value.length) return false;
        return ![...value].some((character) => {
            const code = character.codePointAt(0) ?? 0;
            return code <= 0x20 || code === 0x7f;
        });
    }, '邮箱格式不正确')
    .transform((value) => value || undefined);

export const createUserSchema = z.object({
    username: z.string().min(2, '用户名长度需在 2 - 50 之间').max(50, '用户名长度需在 2 - 50 之间'),
    password: z.string().min(6, '密码长度需在 6 - 100 之间').max(100, '密码长度需在 6 - 100 之间'),
    nickname: z.string().max(100, '昵称不能超过 100 个字符').optional(),
    phone: phoneSchema.optional(),
    email: emailSchema.optional(),
    status: statusSchema.optional(),
    role_ids: z.array(z.uuid({ error: '角色 ID 必须是 UUID' })).min(1, '至少选择一个角色'),
});

export const updateUserSchema = z.object({
    nickname: z.string().max(100, '昵称不能超过 100 个字符').optional(),
    phone: phoneSchema.optional(),
    email: emailSchema.optional(),
    status: statusSchema.optional(),
    password: z
        .union([
            z.literal('').transform(() => undefined),
            z.string().min(6, '密码长度需在 6 - 100 之间').max(100, '密码长度需在 6 - 100 之间'),
        ])
        .optional(),
    role_ids: z
        .array(z.uuid({ error: '角色 ID 必须是 UUID' }))
        .min(1, '至少选择一个角色')
        .optional(),
});

export const userListQuerySchema = pageParamsSchema.extend({
    status: statusSchema.optional(),
});

export const userOptionsQuerySchema = z.object({
    keyword: z.string().max(100, '搜索关键字过长').optional(),
});

export const userIdSchema = z.uuid({ error: 'id 必须是 UUID' });
