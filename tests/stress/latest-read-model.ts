const fixture = (await Bun.file('build/stress-fixture.json').json()) as {
    links: { modbusServer: string };
    devices: { modbusServer: string[] };
    coverage: {
        modbusElementIds: string[];
        latestSamples: Array<{ deviceCode: string; elementId?: string }>;
    };
};
const host = '127.0.0.1';
const deviceCode = 'MS000';
const deviceId = fixture.devices.modbusServer[0];
const parsedStream = 'iot:channel:packet:parsed:worker:0';
const persistenceGroup = 'iot-engine:telemetry-persistence';
const redisUrl = Bun.env.REDIS_PASSWORD
    ? `redis://:${encodeURIComponent(Bun.env.REDIS_PASSWORD)}@${Bun.env.REDIS_HOST ?? host}:${Bun.env.REDIS_PORT ?? '6379'}/${Bun.env.REDIS_DATABASE ?? '0'}`
    : `redis://${Bun.env.REDIS_HOST ?? host}:${Bun.env.REDIS_PORT ?? '6379'}/${Bun.env.REDIS_DATABASE ?? '0'}`;
const redis = new Bun.RedisClient(redisUrl);

let lastTimestamp = 0n;
let sequence = 0n;
const sequenceMask = (1n << 74n) - 1n;

function uuidv7() {
    let timestamp = BigInt(Date.now());
    if (timestamp > lastTimestamp) {
        lastTimestamp = timestamp;
        const bytes = crypto.getRandomValues(new Uint8Array(10));
        sequence = bytes.reduce((value, byte) => (value << 8n) | BigInt(byte), 0n) & sequenceMask;
    } else {
        timestamp = lastTimestamp;
        sequence = (sequence + 1n) & sequenceMask;
        if (sequence === 0n) lastTimestamp++;
        timestamp = lastTimestamp;
    }
    const bytes = new Uint8Array(16);
    let time = timestamp;
    for (let index = 5; index >= 0; index--) {
        bytes[index] = Number(time & 0xffn);
        time >>= 8n;
    }
    let random = sequence;
    for (let index = 15; index >= 9; index--) {
        bytes[index] = Number(random & 0xffn);
        random >>= 8n;
    }
    bytes[8] = 0x80 | Number(random & 0x3fn);
    random >>= 6n;
    bytes[7] = Number(random & 0xffn);
    random >>= 8n;
    bytes[6] = 0x70 | Number(random & 0x0fn);
    const hex = [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function base64Url(value: unknown) {
    return Buffer.from(JSON.stringify(value)).toString('base64url');
}

async function accessToken() {
    const now = Math.floor(Date.now() / 1000);
    const header = base64Url({ alg: 'HS256', typ: 'JWT' });
    const payload = base64Url({
        iss: 'iot-engine',
        aud: 'iot-engine-web',
        sub: '00000000-0000-7000-8000-000000000002',
        exp: now + 3600,
        iat: now,
        user_id: '00000000-0000-7000-8000-000000000002',
        username: 'admin',
        token_type: 'access',
    });
    const key = await crypto.subtle.importKey(
        'raw',
        new TextEncoder().encode(Bun.env.JWT_SECRET ?? ''),
        { name: 'HMAC', hash: 'SHA-256' },
        false,
        ['sign'],
    );
    const signature = await crypto.subtle.sign(
        'HMAC',
        key,
        new TextEncoder().encode(`${header}.${payload}`),
    );
    return `${header}.${payload}.${Buffer.from(signature).toString('base64url')}`;
}

async function hash(key: string) {
    return (await redis.send('HGETALL', [key])) as Record<string, string>;
}

async function waitFor<T>(label: string, read: () => Promise<T>, accept: (value: T) => boolean) {
    const deadline = Date.now() + 10000;
    let value = await read();
    while (!accept(value) && Date.now() < deadline) {
        await Bun.sleep(20);
        value = await read();
    }
    if (!accept(value)) throw new Error(`timed out waiting for ${label}`);
    return value;
}

async function publish(value: number, observedAt: number) {
    const messageId = uuidv7();
    await redis.send('XADD', [
        parsedStream,
        '*',
        'message_id',
        messageId,
        'causation_id',
        uuidv7(),
        'link_id',
        fixture.links.modbusServer,
        'device_id',
        deviceId,
        'device_code',
        deviceCode,
        'protocol',
        'Modbus',
        'connection_id',
        uuidv7(),
        'occurred_at_ms',
        String(observedAt),
        'observed_at_ms',
        String(observedAt),
        'source',
        'integration',
        'values_json',
        JSON.stringify({
            values: {
                [elementId]: { name: element.element_name, value, unit: element.unit },
            },
        }),
        'raw_payload_hex',
        '["0103"]',
    ]);
    return messageId;
}

const elementId = fixture.coverage.latestSamples.find(
    (sample) => sample.deviceCode === deviceCode,
)?.elementId ?? fixture.coverage.modbusElementIds[0];
if (!elementId) throw new Error(`fixture has no latest-value sample for ${deviceCode}`);
const elementKey = `iot:latest:device:${deviceCode}:element:${elementId}`;
const element = await hash(elementKey);
if (!element.element_name) throw new Error('hydrated element metadata is missing');

const newerObservedAt = Date.now();
const olderObservedAt = newerObservedAt - 60_000;
const newerValue = 9001;
const olderValue = 1001;
await publish(newerValue, newerObservedAt);
await waitFor(
    'newer Redis latest value',
    () => hash(elementKey),
    (value) => value.value === String(newerValue) && value.observed_at_ms === String(newerObservedAt),
);
await publish(olderValue, olderObservedAt);
await waitFor(
    'parsed stream acknowledgement',
    async () => {
        const pending = (await redis.send('XPENDING', [parsedStream, persistenceGroup])) as unknown[];
        const depth = Number(await redis.send('XLEN', [parsedStream]));
        return { pending: Number(pending[0]), depth };
    },
    (value) => value.pending === 0 && value.depth === 0,
);

const retained = await hash(elementKey);
if (retained.value !== String(newerValue) || retained.observed_at_ms !== String(newerObservedAt))
    throw new Error('late telemetry overwrote the newer Redis value');
const retainedDocument = JSON.parse(retained.data) as {
    element_id: string;
    value: number;
    observed_at_ms: number;
};
if (
    retainedDocument.element_id !== elementId ||
    retainedDocument.value !== newerValue ||
    retainedDocument.observed_at_ms !== newerObservedAt
)
    throw new Error('Redis latest JSON document does not match the indexed Hash fields');
const state = await hash(`iot:state:device:${deviceCode}`);
if (state.last_report_at_ms !== String(newerObservedAt))
    throw new Error('late telemetry moved the device report timestamp backwards');

const response = await fetch(`http://${host}:1102/api/device`, {
    headers: { Authorization: `Bearer ${await accessToken()}` },
});
const result = (await response.json()) as {
    code: number;
    message: string;
    data?: {
        list: Array<{
            device_code: string;
            reportTime?: string;
            elements: Array<{ id: string; value: string }>;
        }>;
    };
};
if (!response.ok || result.code !== 0) throw new Error(`device API failed: ${result.message}`);
const apiDevice = result.data?.list.find((device) => device.device_code === deviceCode);
const apiElement = apiDevice?.elements.find((item) => item.id === elementId);
if (apiElement?.value !== String(newerValue) || apiDevice?.reportTime !== String(newerObservedAt))
    throw new Error('device API did not return the Redis latest read model');

console.log(
    JSON.stringify(
        {
            deviceCode,
            elementId,
            retainedValue: retained.value,
            retainedObservedAt: retained.observed_at_ms,
            lateObservedAt: olderObservedAt,
            parsedDepth: 0,
            pending: 0,
        },
        null,
        2,
    ),
);
redis.close();
