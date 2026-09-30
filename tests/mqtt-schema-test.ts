import { expect, test } from 'bun:test';
import { mqttConfigSchema } from '../web/pages/iot/protocol/protocol.schema';
import { saveLinkSchema } from '../web/pages/iot/link/link.schema';

const point = {
    id: '00000000-0000-7000-8000-000000000001',
    name: '温度',
    field: '/metrics/temperature',
    dataType: 'DOUBLE' as const,
};
const config = {
    storagePolicy: 'report' as const,
    topic: 'devices/+/telemetry',
    identitySource: 'topic' as const,
    topicDeviceSegment: 1,
    qos: 1 as const,
    points: [point],
};

test('MQTT templates support wildcard topic identity and nested JSON batches', () => {
    expect(mqttConfigSchema.safeParse(config).success).toBe(true);
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            identitySource: 'payload',
            recordsPath: '/devices',
            deviceCodeField: '/meta/code',
        }).success
    ).toBe(true);
    expect(mqttConfigSchema.safeParse({ ...config, identitySource: 'bound' }).success).toBe(false);
    expect(mqttConfigSchema.safeParse({ ...config, topic: 'devices/a+/telemetry' }).success).toBe(
        false
    );
    expect(mqttConfigSchema.safeParse({ ...config, topic: 'devices/#/telemetry' }).success).toBe(
        false
    );
});

test('MQTT text and binary selectors validate against record boundaries', () => {
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            payloadFormat: 'text',
            points: [{ ...point, field: '1' }],
        }).success
    ).toBe(true);
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            payloadFormat: 'text',
            points: [{ ...point, field: '-1' }],
        }).success
    ).toBe(false);
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            payloadFormat: 'binary',
            recordLength: 6,
            points: [{ ...point, field: '4:2:UINT:LE' }],
        }).success
    ).toBe(true);
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            payloadFormat: 'binary',
            recordLength: 5,
            points: [{ ...point, field: '4:2:UINT:LE' }],
        }).success
    ).toBe(false);
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            payloadFormat: 'binary',
            points: [{ ...point, field: '0:8:UINT' }],
        }).success
    ).toBe(false);
});

test('MQTT writable mappings require command topics, invertible scale and unique enums', () => {
    expect(
        mqttConfigSchema.safeParse({ ...config, points: [{ ...point, writable: true }] }).success
    ).toBe(false);
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            commandTopic: 'commands/{deviceCode}',
            points: [{ ...point, writable: true, scale: 0 }],
        }).success
    ).toBe(false);
    expect(
        mqttConfigSchema.safeParse({
            ...config,
            points: [
                {
                    ...point,
                    enumValues: [
                        { input: 'on', output: '1' },
                        { input: 'off', output: '1.0' },
                    ],
                },
            ],
        }).success
    ).toBe(false);
    expect(mqttConfigSchema.safeParse({ ...config, commandTemplate: '{invalid}' }).success).toBe(
        false
    );
});

test('Broker connections accept domains and keep other protocols restricted to IPv4', () => {
    const link = {
        name: 'Broker',
        protocol: 'MQTT',
        status: 'enabled',
        endpoint: {
            mode: 'TCP Client',
            ip: '',
            port: 0,
            targets: [
                {
                    id: 'broker',
                    name: 'Broker',
                    ip: 'mqtt.example.com',
                    port: 1883,
                    status: 'enabled',
                    mqtt: { clientId: 'test-client' },
                },
            ],
        },
    };
    expect(saveLinkSchema.safeParse(link).success).toBe(true);
    expect(saveLinkSchema.safeParse({ ...link, protocol: 'Modbus' }).success).toBe(false);
    expect(
        saveLinkSchema.safeParse({
            ...link,
            endpoint: {
                ...link.endpoint,
                targets: [{ ...link.endpoint.targets[0], mqtt: undefined }],
            },
        }).success
    ).toBe(false);
});
