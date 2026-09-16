import { expect, test } from 'bun:test';
import { serialPayloadHex } from '../web/pages/iot/edge_node/edge_node.service';
import { serialSettingsSchema } from '../web/pages/iot/edge_node/edge_node.schema';

test('serial HEX preserves binary bytes and ignores only whitespace', () => {
    expect(serialPayloadHex('00 ff\n0a 80', 'hex')).toBe('00FF0A80');
    for (const value of ['', '0', '0xFF', 'GG', '00,FF', '00'.repeat(1025)])
        expect(() => serialPayloadHex(value, 'hex')).toThrow();
});

test('serial text sends UTF-8 and exactly the requested line ending', () => {
    expect(serialPayloadHex('测试', 'text', '\r\n')).toBe('E6B58BE8AF950D0A');
    expect(serialPayloadHex('\0', 'text')).toBe('00');
    expect(() => serialPayloadHex('测'.repeat(342), 'text')).toThrow();
    expect(serialPayloadHex('FF'.repeat(512), 'text').length).toBe(2048);
});

test('serial settings reject unsupported rates and parity', () => {
    const settings = { baudRate: 9600, dataBits: 8, stopBits: 1, parity: 'none', rs485: true };
    expect(serialSettingsSchema.safeParse(settings).success).toBe(true);
    expect(serialSettingsSchema.safeParse({ ...settings, baudRate: 9601 }).success).toBe(false);
    expect(serialSettingsSchema.safeParse({ ...settings, parity: 'mark' }).success).toBe(false);
});
