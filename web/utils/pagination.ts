import { z } from 'zod';

export const pageParamsSchema = z.object({
    page: z.coerce.number().int().min(1, 'page 必须大于 0').optional(),
    pageSize: z.coerce
        .number()
        .int()
        .min(1, 'pageSize 必须在 1 - 100 之间')
        .max(100, 'pageSize 必须在 1 - 100 之间')
        .optional(),
    keyword: z.string().optional(),
});
