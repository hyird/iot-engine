import { expect, test } from 'bun:test';

import { numberOrDefault } from '../web/pages/iot/protocol/modbus/helpers';
import { protocolCreateSchema } from '../web/pages/iot/protocol/protocol.schema';

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
