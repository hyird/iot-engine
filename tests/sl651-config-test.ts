import { expect, test } from 'bun:test';
import { protocolCreateSchema, protocolUpdateSchema } from '../web/pages/iot/protocol/protocol.schema';

const config = (guideHex: string, length: number, digits: number) => ({
    responseMode: 'M3', storagePolicy: 'report',
    funcs: [{ id: 'custom-function', funcCode: 'E1', dir: 'UP', name: '扩展数据',
        elements: [{ id: 'custom-element', name: '自定义量', guideHex, encode: 'BCD', length, digits }] }],
});

test('SL651 custom and extended identifiers follow the wire data definition', () => {
    for (const value of [config('AB12', 2, 2), config('FFB028', 5, 0)]) {
        expect(protocolCreateSchema.safeParse({ name: '自定义协议', protocol: 'SL651', config: value }).success).toBe(true);
        expect(protocolUpdateSchema.safeParse({ config: value }).success).toBe(true);
    }
});

test('SL651 create and update reject mismatched guides, lengths and precision', () => {
    for (const value of [config('3912', 3, 2), config('3912', 2, 3), config('01', 1, 0), config('FF12', 2, 2), config('AB18', 3, 8)]) {
        expect(protocolCreateSchema.safeParse({ name: '错误协议', protocol: 'SL651', config: value }).success).toBe(false);
        expect(protocolUpdateSchema.safeParse({ config: value }).success).toBe(false);
    }
});


test('SL651 fixed positions do not require a guide and validate byte bounds', () => {
    const fixed = config('', 2, 2);
    Object.assign(fixed.funcs[0].elements[0], {positionMode: 'OFFSET', byteOffset: 8});
    expect(protocolUpdateSchema.safeParse({config: fixed}).success).toBe(true);
    for (const byteOffset of [-1, 0.5, 8388607]) {
        Object.assign(fixed.funcs[0].elements[0], {byteOffset});
        expect(protocolUpdateSchema.safeParse({config: fixed}).success).toBe(false);
    }
});
