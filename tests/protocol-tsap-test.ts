import { expect, test } from 'bun:test';
import { formatTsapValue, validateTsapValue } from '../web/pages/iot/protocol/protocol.schema';

test('TSAP form and connection formatting share normalization', async () => {
    for (const [input, expected] of [['a', '000A'], ['0x4d57', '4D57'], ['01:00', '0100'], ['0_2-0.0', '0200'], [' 1 ', '0001']]) {
        expect(formatTsapValue(input)).toBe(expected);
        await expect(validateTsapValue(undefined, input)).resolves.toBeUndefined();
    }
});

test('TSAP form rejects absent, malformed and oversized hexadecimal values', async () => {
    for (const input of [undefined, '', '0x', '---', 'GG', '10000', '1z', ' 0x01']) {
        expect(formatTsapValue(input)).toBeUndefined();
        await expect(validateTsapValue(undefined, input)).rejects.toThrow('请输入 1-4 位十六进制 TSAP');
    }
});
