import { expect, test } from 'bun:test';

import { numberOrDefault } from '../web/pages/iot/protocol/protocol.service';
import {
    derivedPointSchema,
    protocolCreateSchema,
    validatePointExpression,
} from '../web/pages/iot/protocol/protocol.schema';

test('公式校验接受运行时支持的语法', () => {
    for (const expression of [
        'x',
        'if(x > 10, x * 2, 0)',
        'min(x, max(y, 0))',
        'abs(-x) + sqrt(y) + round(.5)',
        '!(x == 0) && (y != 1 || true)',
        '1.2e-3 + +x % 2',
    ]) {
        expect(() => validatePointExpression(expression, ['x', 'y'])).not.toThrow();
    }
});

test('公式校验拒绝语法错误、未知变量函数及复杂度越界', () => {
    for (const expression of [
        '',
        'x +',
        '(x + 1',
        'x 2',
        'x = 2',
        'x ** 2',
        'if(x, 1)',
        'min(x)',
        'sqrt(x, y)',
        'sin(x)',
        'constructor(x)',
        'missing + x',
        '1e999',
        'NaN',
        'x; alert(1)',
        '('.repeat(26) + 'x' + ')'.repeat(26),
        Array(66).fill('x').join('+'),
        'x'.repeat(513),
    ]) {
        expect(() => validatePointExpression(expression, ['x', 'y'])).toThrow();
    }
});

test('派生值将公式和单位条件错误定位到字段，修改绑定后重新校验', () => {
    const point = {
        id: '00000000-0000-4000-8000-000000000001',
        name: '压力',
        kind: 'expression',
        valueType: 'number',
        unitMode: 'conditional',
        expression: 'x +',
        unitRules: [{ condition: 'unknown > 1', unit: 'MPa' }],
        inputs: [{ alias: 'x', pointId: '00000000-0000-4000-8000-000000000002' }],
        maxAgeSeconds: 300,
        visible: false,
    };
    const invalid = derivedPointSchema.safeParse(point);
    expect(invalid.success).toBe(false);
    if (!invalid.success)
        expect(invalid.error.issues.map((issue) => issue.path)).toEqual([
            ['expression'],
            ['unitRules', 0, 'condition'],
        ]);
    const valid = {
        ...point,
        expression: 'x / 1000',
        unitRules: [{ condition: 'x > 1000', unit: 'MPa' }],
    };
    expect(derivedPointSchema.safeParse(valid).success).toBe(true);
    expect(
        derivedPointSchema.safeParse({ ...valid, inputs: [{ ...point.inputs[0], alias: 'y' }] })
            .success
    ).toBe(false);
});

test('S7 protocol schema rejects malformed area fields', () => {
    const result = protocolCreateSchema.safeParse({
        name: 's7-test',
        protocol: 'S7',
        config: {
            plcModel: 'S7-1200',
            connection: {},
            areas: [
                {
                    id: 'temperature',
                    name: 'Temperature',
                    area: 'DB',
                    dataType: 'INT16',
                    dbNumber: 1,
                    start: 'bad',
                    size: 2,
                },
            ],
        },
    });

    expect(result.success).toBe(false);
});

test('Modbus protocol schema rejects fractional register addresses', () => {
    const result = protocolCreateSchema.safeParse({
        name: 'modbus-test',
        protocol: 'Modbus',
        config: {
            byteOrder: 'BIG_ENDIAN',
            registers: [
                {
                    id: 'r1',
                    name: 'Register 1',
                    registerType: 'HOLDING_REGISTER',
                    dataType: 'INT16',
                    address: 1.5,
                    quantity: 1,
                },
            ],
        },
    });

    expect(result.success).toBe(false);
});

test('Modbus helpers normalize legacy numeric strings before save', () => {
    expect(numberOrDefault('5', 1)).toBe(5);
    expect(numberOrDefault('bad', 1)).toBe(1);
});
