import { describe, expect, test } from 'bun:test';
import {
    logLevelSchema,
    logsQuerySchema,
} from '../web/pages/iot/edge_node/edge_node.schema';

describe('节点日志级别', () => {
    test('支持静默设置并保留旧级别', () => {
        for (const level of ['silent', 'debug', 'info', 'warn', 'error']) {
            expect(logLevelSchema.parse({ level })).toEqual({ level });
        }
    });
    test('静默不是日志记录的筛选等级', () => {
        expect(logsQuerySchema.safeParse({ level: 'silent' }).success).toBe(false);
        expect(logsQuerySchema.safeParse({ level: 'error' }).success).toBe(true);
    });
    test('拒绝拼写错误和缺失级别', () => {
        for (const input of [{ level: 'slient' }, { level: '' }, {}, { level: null }]) {
            expect(logLevelSchema.safeParse(input).success).toBe(false);
        }
    });
});
