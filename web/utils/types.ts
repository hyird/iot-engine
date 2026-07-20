/**
 * 前端公共类型定义
 */

export interface PageParams {
    page?: number | string;
    pageSize?: number | string;
    keyword?: string;
}

export interface PaginatedResult<T> {
    list: T[];
    total: number;
    page?: number;
    pageSize?: number;
    totalPages?: number;
}

export interface NormalizedPagination {
    page: number;
    pageSize: number;
    skip: number;
    keyword?: string;
}

export type Status = 'enabled' | 'disabled';

// ============ Zod Schemas ============

import { z } from 'zod';

export const pageParamsSchema = z.object({
    page: z
        .union([z.string(), z.number()])
        .optional()
        .transform((val) => {
            if (val === undefined || val === '') return 1;
            return Number(val);
        }),
    pageSize: z
        .union([z.string(), z.number()])
        .optional()
        .transform((val) => {
            if (val === undefined || val === '') return 10;
            return Number(val);
        }),
    keyword: z.string().optional(),
});

export type PageParamsInput = z.infer<typeof pageParamsSchema>;
