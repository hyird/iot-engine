import { describe, expect, test } from 'bun:test';
import { validateValue } from '../web/pages/iot/device/CommandPopover';

describe('device command validation', () => {
    test('rejects negative BCD values because the wire format has no sign', () => {
        expect(
            validateValue({
                _key: 'level',
                elementId: 'level',
                name: '水位',
                value: '-12.34',
                encode: 'BCD',
                length: 2,
                digits: 2,
            })
        ).not.toBeNull();
    });

    test('measures STRING limits in UTF-8 bytes', () => {
        expect(
            validateValue({
                _key: 'name',
                elementId: 'name',
                name: '名称',
                value: '水位',
                dataType: 'STRING',
                size: 2,
            })
        ).not.toBeNull();
    });

    test('rejects JavaScript-only numeric literals', () => {
        expect(
            validateValue({
                _key: 'bcd',
                elementId: 'bcd',
                name: 'BCD',
                value: '0x10',
                encode: 'BCD',
                length: 2,
            })
        ).not.toBeNull();
        expect(
            validateValue({
                _key: 'float',
                elementId: 'float',
                name: '浮点数',
                value: '0b10',
                dataType: 'FLOAT32',
            })
        ).not.toBeNull();
    });
});
