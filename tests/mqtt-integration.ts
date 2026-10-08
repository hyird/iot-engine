import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { apiBase, databaseUrl } from './architecture-fixture';

const db = new Bun.SQL(databaseUrl);
const admin = '00000000-0000-7000-8000-000000000002';
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({ iss: 'iot-engine', aud: 'iot-engine-web', sub: admin, user_id: admin, username: 'admin', token_type: 'access', iat: now, exp: now + 3600 })}`;
const token = `${unsigned}.${createHmac('sha256', 'architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;
const point = crypto.randomUUID(),
    enabledPoint = crypto.randomUUID(),
    target = crypto.randomUUID();
const name = `mqtt-${crypto.randomUUID()}`;
let link = '',
    model = '',
    published = false,
    acknowledgements = 0,
    commands = 0;
const devices: string[] = [];
const clients = new Set<Bun.Socket<{ buffer: Buffer; subscribed: boolean }>>();
const time = Date.now();

function packet(header: number, body: Buffer) {
    const length: number[] = [];
    let remaining = body.length;
    do {
        let byte = remaining % 128;
        remaining = Math.floor(remaining / 128);
        if (remaining) byte |= 128;
        length.push(byte);
    } while (remaining);
    return Buffer.concat([Buffer.from([header, ...length]), body]);
}
function text(input: string) {
    const value = Buffer.from(input);
    const length = Buffer.alloc(2);
    length.writeUInt16BE(value.length);
    return Buffer.concat([length, value]);
}
function telemetry(socket: Bun.Socket<{ buffer: Buffer; subscribed: boolean }>) {
    if (!published || !socket.data.subscribed) return;
    const payload = {
        devices: [
            { code: 'D001', metrics: { temperature: 256 }, time },
            { code: 'D002', metrics: { temperature: 281 }, time },
        ],
    };
    const frame = packet(
        0x32,
        Buffer.concat([
            text('factory/telemetry'),
            Buffer.from([0, 7]),
            Buffer.from(JSON.stringify(payload)),
        ])
    );
    socket.write(frame.subarray(0, 3));
    socket.write(frame.subarray(3));
}
const broker = Bun.listen<{ buffer: Buffer; subscribed: boolean }>({
    hostname: '127.0.0.1',
    port: 0,
    socket: {
        open(socket) {
            socket.data = { buffer: Buffer.alloc(0), subscribed: false };
            clients.add(socket);
        },
        data(socket, data) {
            let buffer = Buffer.concat([socket.data.buffer, Buffer.from(data)]);
            for (;;) {
                if (buffer.length < 2) break;
                let length = 0,
                    position = 1,
                    multiplier = 1,
                    complete = false;
                for (let count = 0; count < 4 && position < buffer.length; ++count) {
                    const byte = buffer[position++];
                    length += (byte & 127) * multiplier;
                    multiplier *= 128;
                    if (!(byte & 128)) {
                        complete = true;
                        break;
                    }
                }
                if (!complete || buffer.length < position + length) break;
                const header = buffer[0],
                    body = buffer.subarray(position, position + length);
                buffer = buffer.subarray(position + length);
                if (header === 0x10) {
                    assert.equal(body.subarray(2, 6).toString(), 'MQTT');
                    assert.equal(body[6], 4);
                    socket.write(Buffer.from([0x20, 2, 0, 0]));
                } else if (header === 0x82) {
                    assert.equal(body.subarray(4, -1).toString(), 'factory/telemetry');
                    socket.data.subscribed = true;
                    socket.write(Buffer.from([0x90, 3, body[0], body[1], 1]));
                    telemetry(socket);
                } else if (header === 0x40) {
                    assert.equal(body.readUInt16BE(), 7);
                    ++acknowledgements;
                } else if (header === 0xc0) socket.write(Buffer.from([0xd0, 0]));
                else if (header >> 4 === 3) {
                    const size = body.readUInt16BE(),
                        topic = body.subarray(2, 2 + size).toString();
                    assert.equal(topic, 'factory/commands');
                    const payload = JSON.parse(body.subarray(4 + size).toString());
                    assert.deepEqual(payload, {
                        cmd: 'set',
                        device: 'D001',
                        control: { targetTemperature: 150, enabled: false },
                    });
                    ++commands;
                    socket.write(Buffer.from([0x40, 2, body[2 + size], body[3 + size]]));
                } else throw Error(`unexpected MQTT packet ${header}`);
            }
            socket.data.buffer = Buffer.from(buffer);
        },
        close(socket) {
            clients.delete(socket);
        },
        error(_socket, error) {
            throw error;
        },
    },
});

async function http(method: string, path: string, data?: unknown, auth = true) {
    const response = await fetch(apiBase + path, {
        method,
        headers: {
            ...(auth ? { Authorization: `Bearer ${token}` } : {}),
            'Content-Type': 'application/json',
        },
        body: data === undefined ? undefined : JSON.stringify(data),
        signal: AbortSignal.timeout(15000),
    });
    return { status: response.status, body: await response.json() };
}
async function ok(method: string, path: string, data?: unknown) {
    const response = await http(method, path, data);
    assert.equal(response.status, 200, JSON.stringify(response.body));
    assert.equal(response.body.code, 0, JSON.stringify(response.body));
    return response.body.data;
}
async function until(check: () => Promise<boolean>, reason: string) {
    const deadline = Date.now() + 25000;
    while (Date.now() < deadline) {
        if (await check()) return;
        await Bun.sleep(50);
    }
    throw Error(reason);
}

try {
    const config = {
        storagePolicy: 'report',
        topic: 'factory/telemetry',
        payloadFormat: 'json',
        recordsPath: '/devices',
        identitySource: 'payload',
        deviceCodeField: '/code',
        timeField: '/time',
        timeFormat: 'unix_ms',
        commandTopic: 'factory/commands',
        commandTemplate: '{"cmd":"set","device":"$deviceCode","control":{"targetTemperature":"$point:温度","enabled":"$point:开关"}}',
        qos: 1,
        points: [
            {
                id: point,
                name: '温度',
                field: '/metrics/temperature',
                dataType: 'DOUBLE',
                unit: '℃',
                scale: 0.1,
                writable: true,
            },
            {
                id: enabledPoint,
                name: '开关',
                field: '/metrics/enabled',
                dataType: 'BOOL',
                writable: true,
            },
        ],
    };
    assert.equal(
        (
            await http(
                'POST',
                '/v1/protocol/configs/preview-mqtt',
                { config, topic: 'factory/telemetry', payload: '{}' },
                false
            )
        ).status,
        401
    );
    const preview = await ok('POST', '/v1/protocol/configs/preview-mqtt', {
        config,
        topic: 'factory/telemetry',
        payload: JSON.stringify({
            devices: [{ code: 'D001', metrics: { temperature: 256 }, time }],
        }),
    });
    assert.equal(preview.records[0].deviceCode, 'D001');
    assert.equal(preview.records[0].points[0].value, '25.6');
    await ok('POST', '/v1/protocol/configs', { protocol: 'MQTT', name, enabled: true, config });
    model = (await db`SELECT id FROM protocol_config WHERE name=${name}`)[0].id;
    await ok('POST', '/v1/link', {
        name,
        protocol: 'MQTT',
        status: 'enabled',
        endpoint: {
            mode: 'TCP Client',
            ip: '',
            port: 0,
            targets: [
                {
                    id: target,
                    name: 'Broker',
                    ip: 'localhost',
                    port: broker.port,
                    status: 'enabled',
                    mqtt: { clientId: name, keepAliveSeconds: 30 },
                },
            ],
        },
    });
    link = (await db`SELECT id FROM link WHERE name=${name}`)[0].id;
    for (const code of ['D001', 'D002']) {
        await ok('POST', '/v1/device', {
            name: `${name}-${code}`,
            device_code: code,
            link_id: link,
            target_id: target,
            protocol_config_id: model,
            online_timeout: 60,
            remote_control: true,
            timezone: '+08:00',
            status: 'enabled',
        });
        devices.push((await db`SELECT id FROM device WHERE name=${`${name}-${code}`}`)[0].id);
    }
    published = true;
    for (const socket of clients) telemetry(socket);
    await until(
        async () =>
            (
                await db`SELECT DISTINCT device_id FROM device_data WHERE device_id IN (${devices[0]},${devices[1]}) AND protocol='MQTT'`
            ).length === 2,
        'MQTT batch did not reach both devices'
    );
    for (const [index, id] of devices.entries()) {
        const [row] =
            await db`SELECT data->'values'->${point}->>'value' AS value, EXTRACT(EPOCH FROM occurred_at)*1000 AS time FROM device_data WHERE device_id=${id} ORDER BY occurred_at DESC LIMIT 1`;
        assert(Math.abs(Number(row.value) - (index === 0 ? 25.6 : 28.1)) < 0.00001);
        assert.equal(Number(row.time), time);
    }
    await until(async () => acknowledgements > 0, 'Broker did not receive PUBACK');
    const command = await ok('POST', `/v1/device/${devices[0]}/commands`, {
        idempotency_key: crypto.randomUUID(),
        elements: [{ elementId: point, value: '15' }, { elementId: enabledPoint, value: '0' }],
    });
    assert.equal(command.command_ids.length, 1, 'one MQTT message must have one acknowledgement');
    await until(async () => commands === 1, 'MQTT command was not sent');
    await until(
        async () =>
            (await db`SELECT status FROM command_operation WHERE id=${command.command_ids[0]}`)[0]
                ?.status === 'SUCCEEDED',
        'MQTT command did not complete after PUBACK'
    );
    console.log(
        `PASS MQTT full integration: two devices, nested batch, device time, preview auth, command template, Broker acknowledgement`
    );
} finally {
    for (const device of devices) await http('DELETE', `/v1/device/${device}`);
    if (link) await http('DELETE', `/v1/link/${link}`);
    if (model) await http('DELETE', `/v1/protocol/configs/${model}`);
    for (const socket of clients) socket.end();
    broker.stop(true);
    await db.close();
}
