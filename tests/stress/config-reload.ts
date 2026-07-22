import net from 'node:net';

const fixture = await Bun.file('build/stress-fixture.json').json();
const host = '127.0.0.1';
const redisUrl = Bun.env.REDIS_PASSWORD
    ? `redis://:${encodeURIComponent(Bun.env.REDIS_PASSWORD)}@${Bun.env.REDIS_HOST ?? host}:${Bun.env.REDIS_PORT ?? '6379'}/${Bun.env.REDIS_DATABASE ?? '0'}`
    : `redis://${Bun.env.REDIS_HOST ?? host}:${Bun.env.REDIS_PORT ?? '6379'}/${Bun.env.REDIS_DATABASE ?? '0'}`;
const redis = new Bun.RedisClient(redisUrl);
const sockets = new Set<net.Socket>();
const timers = new Set<Timer>();
const deviceCode = `CFG${Date.now().toString(36).toUpperCase()}`;
const batchPrefix = `CFGB${Date.now().toString(36).toUpperCase()}`;
let createdDeviceId = '';
const batchDeviceIds = new Set<string>();

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

const token = await accessToken();
async function api(method: string, path: string, body?: unknown) {
    const response = await fetch(`http://${host}:1102${path}`, {
        method,
        headers: {
            Authorization: `Bearer ${token}`,
            ...(body ? { 'Content-Type': 'application/json' } : {}),
        },
        body: body ? JSON.stringify(body) : undefined,
    });
    const result = (await response.json()) as { code: number; message: string; data?: unknown };
    if (!response.ok || result.code !== 0)
        throw new Error(`${method} ${path} failed: ${response.status} ${result.message}`);
    return result;
}

async function expectConflict(path: string, message: string) {
    const response = await fetch(`http://${host}:1102${path}`, {
        method: 'DELETE',
        headers: { Authorization: `Bearer ${token}` },
    });
    const result = (await response.json()) as { code: number; message: string };
    if (response.status !== 409 || !result.message.includes(message))
        throw new Error(`DELETE ${path} should be blocked by device reference`);
}

async function hash(key: string) {
    return (await redis.send('HGETALL', [key])) as Record<string, string>;
}

async function scan(pattern: string) {
    let cursor = '0';
    const result: string[] = [];
    do {
        const page = (await redis.send('SCAN', [
            cursor,
            'MATCH',
            pattern,
            'COUNT',
            '500',
        ])) as [string, string[]];
        cursor = page[0];
        result.push(...page[1]);
    } while (cursor !== '0');
    return result;
}

async function streamDepth(pattern: string) {
    const keys = await scan(pattern);
    const depths = await Promise.all(keys.map((key) => redis.send('XLEN', [key])));
    return depths.reduce((total, depth) => total + Number(depth), 0);
}

async function waitFor<T>(label: string, read: () => Promise<T>, accept: (value: T) => boolean) {
    const deadline = Date.now() + 10000;
    let value = await read();
    while (!accept(value) && Date.now() < deadline) {
        await Bun.sleep(25);
        value = await read();
    }
    if (!accept(value)) throw new Error(`timed out waiting for ${label}`);
    return value;
}

async function activeVersion(previous = '') {
    return waitFor(
        'new active config version',
        async () => String((await redis.send('GET', ['iot:config:runtime:active-version'])) ?? ''),
        (value) => Boolean(value) && value !== previous,
    );
}

async function waitForWorkers(version: string) {
    const initialKeys = (await waitFor(
        'South Worker runtime config states',
        async () =>
            (await redis.send('KEYS', ['iot:state:runtime-config:worker:*'])) as string[],
        (keys) => keys.length > 0,
    )) as string[];
    await waitFor(
        `all ${initialKeys.length} South Workers to apply ${version}`,
        async () => Promise.all(initialKeys.map(async (key) => (await hash(key)).version ?? '')),
        (versions) => versions.length === initialKeys.length && versions.every((value) => value === version),
    );
}

async function route(code: string) {
    return hash(`iot:state:device:${code}`);
}

function track(socket: net.Socket) {
    sockets.add(socket);
    socket.setNoDelay(true);
    socket.on('close', () => sockets.delete(socket));
    return socket;
}

async function connect(port: number) {
    const socket = track(net.createConnection({ host, port }));
    await new Promise<void>((resolve, reject) => {
        socket.once('connect', resolve);
        socket.once('error', reject);
    });
    return socket;
}

function crc16(bytes: Uint8Array) {
    let crc = 0xffff;
    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
    }
    return crc;
}

function sl651Frame() {
    const frame = Buffer.from([
        0x7e, 0x7e, 1, 0, 0, 0, 0, 1, 0, 0, 0x30, 0, 4, 2, 0x39, 0, 0x12, 0x34, 3,
    ]);
    const crc = crc16(frame);
    return Buffer.concat([frame, Buffer.from([crc >> 8, crc & 0xff])]);
}

function attachModbus(socket: net.Socket) {
    let buffered = Buffer.alloc(0);
    socket.on('data', (chunk) => {
        buffered = Buffer.concat([buffered, chunk]);
        while (buffered.length >= 8) {
            const tcp = buffered.length >= 12 && buffered[2] === 0 && buffered[3] === 0;
            const length = tcp ? 6 + buffered.readUInt16BE(4) : 8;
            if (buffered.length < length) break;
            const request = buffered.subarray(0, length);
            buffered = buffered.subarray(length);
            if (tcp) {
                const fn = request[7];
                socket.write(
                    fn === 3
                        ? Buffer.from([request[0], request[1], 0, 0, 0, 5, request[6], fn, 2, 0x12, 0x34])
                        : request,
                );
            }
        }
    });
}

function attachS7(socket: net.Socket) {
    let buffered = Buffer.alloc(0);
    socket.on('data', (chunk) => {
        buffered = Buffer.concat([buffered, chunk]);
        while (buffered.length >= 4) {
            if (buffered[0] !== 3 || buffered[1] !== 0) {
                buffered = buffered.subarray(1);
                continue;
            }
            const length = buffered.readUInt16BE(2);
            if (buffered.length < length) break;
            const frame = buffered.subarray(0, length);
            buffered = buffered.subarray(length);
            if (frame[5] === 0xe0) socket.write(Buffer.from([3, 0, 0, 7, 2, 0xd0, 0]));
            else if (frame[17] === 0xf0)
                socket.write(
                    Buffer.from([
                        3, 0, 0, 0x1b, 2, 0xf0, 0x80, 0x32, 3, 0, 0, frame[11], frame[12], 0,
                        8, 0, 0, 0, 0, 0xf0, 0, 0, 1, 0, 1, 1, 0xe0,
                    ]),
                );
        }
    });
}

async function databaseDeviceId() {
    const process = Bun.spawn(
        [
            'docker',
            'exec',
            '-e',
            `PGPASSWORD=${Bun.env.DB_PASSWORD ?? ''}`,
            'timescaledb',
            'psql',
            '-U',
            Bun.env.DB_USERNAME ?? 'postgres',
            '-d',
            Bun.env.DB_DATABASE ?? 'postgres',
            '-Atc',
            `SELECT id::text FROM device WHERE protocol_params->>'device_code' = '${deviceCode}' AND deleted_at IS NULL`,
        ],
        { stdout: 'pipe', stderr: 'inherit' },
    );
    const id = (await new Response(process.stdout).text()).trim();
    if ((await process.exited) !== 0 || !id) throw new Error('created device was not committed');
    return id;
}

async function databaseDeviceDeleted(id: string) {
    const process = Bun.spawn(
        [
            'docker',
            'exec',
            '-e',
            `PGPASSWORD=${Bun.env.DB_PASSWORD ?? ''}`,
            'timescaledb',
            'psql',
            '-U',
            Bun.env.DB_USERNAME ?? 'postgres',
            '-d',
            Bun.env.DB_DATABASE ?? 'postgres',
            '-Atc',
            `SELECT (deleted_at IS NOT NULL)::text FROM device WHERE id = '${id}'::uuid`,
        ],
        { stdout: 'pipe', stderr: 'inherit' },
    );
    const deleted = (await new Response(process.stdout).text()).trim();
    return (await process.exited) === 0 && deleted === 'true';
}

async function databaseBatchDevices() {
    const process = Bun.spawn(
        [
            'docker',
            'exec',
            '-e',
            `PGPASSWORD=${Bun.env.DB_PASSWORD ?? ''}`,
            'timescaledb',
            'psql',
            '-U',
            Bun.env.DB_USERNAME ?? 'postgres',
            '-d',
            Bun.env.DB_DATABASE ?? 'postgres',
            '-AtF',
            '|',
            '-c',
            `SELECT protocol_params->>'device_code', id::text FROM device WHERE protocol_params->>'device_code' LIKE '${batchPrefix}%' AND deleted_at IS NULL ORDER BY protocol_params->>'device_code'`,
        ],
        { stdout: 'pipe', stderr: 'inherit' },
    );
    const rows = (await new Response(process.stdout).text()).trim();
    if ((await process.exited) !== 0) throw new Error('batch device query failed');
    return new Map(
        rows
            .split('\n')
            .filter(Boolean)
            .map((row) => row.split('|') as [string, string]),
    );
}

const body = (
    status: 'enabled' | 'disabled',
    mode: 'TCP' | 'RTU',
    code = deviceCode,
    slaveId = 101,
) => ({
    name: `Stress Config Reload Device ${code}`,
    device_code: code,
    link_id: fixture.links.modbusServer,
    protocol_config_id: fixture.protocols.Modbus,
    status,
    online_timeout: 30,
    remote_control: true,
    modbus_mode: mode,
    slave_id: slaveId,
    timezone: '+00:00',
    heartbeat: { mode: 'ASCII', content: `H${code}` },
    registration: { mode: 'ASCII', content: code },
    remark: 'configuration reload integration test',
});

try {
    await expectConflict(`/api/link/${fixture.links.s7Server}`, '链路已被设备使用');
    await expectConflict(`/api/protocol/configs/${fixture.protocols.S7}`, '协议配置已被设备使用');

    const modbus = await connect(fixture.serverPorts.modbus);
    attachModbus(modbus);
    modbus.write('M000');
    const s7 = await connect(fixture.serverPorts.s7);
    attachS7(s7);
    s7.write('S000');
    const sl651 = await connect(fixture.serverPorts.sl651);
    sl651.write(sl651Frame());
    const heartbeat = setInterval(() => {
        if (!modbus.destroyed) modbus.write('HM000');
        if (!s7.destroyed) s7.write('HS000');
        if (!sl651.destroyed) sl651.write(sl651Frame());
    }, 1000);
    timers.add(heartbeat);

    const modbusBefore = await waitFor('Modbus route', () => route('MS000'), (value) => Boolean(value.connection_id));
    const s7Before = await waitFor('S7 route', () => route('SS000'), (value) => Boolean(value.connection_id));
    const sl651Before = await waitFor('SL651 route', () => route('0000000001'), (value) => Boolean(value.connection_id));
    let version = await activeVersion();
    await waitForWorkers(version);

    await api('POST', '/api/device', body('enabled', 'TCP'));
    createdDeviceId = await databaseDeviceId();
    version = await activeVersion(version);
    await waitForWorkers(version);
    const projectedTcp = await hash(`iot:config:runtime:${version}:device:${createdDeviceId}`);
    if (projectedTcp.modbus_mode !== 'TCP') throw new Error('created TCP mode was not projected');
    const listResult = await api('GET', '/api/device');
    const createdInList = (
        listResult.data as {
            list: Array<{ device_code: string; elements: Array<{ value: string }> }>;
        }
    ).list.find((device) => device.device_code === deviceCode);
    if (!createdInList?.elements.length || createdInList.elements.some((element) => element.value !== '-'))
        throw new Error('device without telemetry did not expose configured elements as -');
    await waitFor('affected Modbus connection close', async () => modbus.destroyed, Boolean);
    const s7AfterCreate = await route('SS000');
    const sl651AfterCreate = await route('0000000001');
    if (s7AfterCreate.connection_id !== s7Before.connection_id)
        throw new Error('unrelated S7 link was restarted');
    if (sl651AfterCreate.connection_id !== sl651Before.connection_id)
        throw new Error('unrelated SL651 link was restarted');

    const configuredTcp = await connect(fixture.serverPorts.modbus);
    attachModbus(configuredTcp);
    configuredTcp.write(deviceCode);
    await waitFor('new TCP device route', () => route(deviceCode), (value) => Boolean(value.connection_id));

    await api('PUT', `/api/device/${createdDeviceId}`, body('enabled', 'RTU'));
    version = await activeVersion(version);
    await waitForWorkers(version);
    const projectedRtu = await hash(`iot:config:runtime:${version}:device:${createdDeviceId}`);
    if (projectedRtu.modbus_mode !== 'RTU') throw new Error('updated RTU mode was not projected');
    await waitFor('mode-change connection close', async () => configuredTcp.destroyed, Boolean);

    const configuredRtu = await connect(fixture.serverPorts.modbus);
    configuredRtu.write(deviceCode);
    await waitFor('new RTU device route', () => route(deviceCode), (value) => Boolean(value.connection_id));
    await api('PUT', `/api/device/${createdDeviceId}`, body('disabled', 'RTU'));
    version = await activeVersion(version);
    await waitForWorkers(version);
    await waitFor('disabled device route removal', () => route(deviceCode), (value) => !value.connection_id);
    await waitFor('disabled device connection close', async () => configuredRtu.destroyed, Boolean);
    const disabledSnapshot = await hash(`iot:config:runtime:${version}:device:${createdDeviceId}`);
    if (Object.keys(disabledSnapshot).length !== 0)
        throw new Error('disabled device remained in runtime snapshot');

    const deletedDeviceId = createdDeviceId;
    await api('DELETE', `/api/device/${deletedDeviceId}`);
    createdDeviceId = '';
    await waitFor('database soft delete', () => databaseDeviceDeleted(deletedDeviceId), Boolean);
    await Bun.sleep(500);
    const versionAfterDelete = String(
        (await redis.send('GET', ['iot:config:runtime:active-version'])) ?? '',
    );
    if (versionAfterDelete !== version)
        throw new Error('deleting an already disabled device created a redundant runtime version');
    await waitForWorkers(version);

    const batchCodes = Array.from({ length: 8 }, (_, index) =>
        `${batchPrefix}${index.toString().padStart(2, '0')}`,
    );
    await Promise.all(
        batchCodes.map((code, index) =>
            api('POST', '/api/device', body('enabled', 'TCP', code, 120 + index)),
        ),
    );
    const createdBatch = await waitFor(
        'all concurrent database creates',
        databaseBatchDevices,
        (devices) => devices.size === batchCodes.length,
    );
    for (const id of createdBatch.values()) batchDeviceIds.add(id);
    version = await activeVersion(version);
    await waitForWorkers(version);
    for (const [code, id] of createdBatch) {
        const projected = await hash(`iot:config:runtime:${version}:device:${id}`);
        if (projected.code !== code || projected.modbus_mode !== 'TCP')
            throw new Error(`concurrent create was lost from active snapshot: ${code}`);
    }

    await Promise.all(
        [...createdBatch].map(([code, id], index) =>
            api('PUT', `/api/device/${id}`, body('disabled', 'TCP', code, 120 + index)),
        ),
    );
    version = await activeVersion(version);
    await waitForWorkers(version);
    for (const id of createdBatch.values()) {
        if (Object.keys(await hash(`iot:config:runtime:${version}:device:${id}`)).length !== 0)
            throw new Error(`concurrently disabled device remained active: ${id}`);
    }

    await Promise.all([...createdBatch.values()].map((id) => api('DELETE', `/api/device/${id}`)));
    batchDeviceIds.clear();
    await Bun.sleep(500);
    const batchDeleteVersion = String(
        (await redis.send('GET', ['iot:config:runtime:active-version'])) ?? '',
    );
    if (batchDeleteVersion !== version)
        throw new Error('batch deleting disabled devices created a redundant runtime version');
    await waitFor(
        'config notification stream cleanup',
        () => streamDepth('iot:channel:config:worker:*'),
        (depth) => depth === 0,
    );
    const s7Final = await route('SS000');
    const sl651Final = await route('0000000001');
    if (s7Final.connection_id !== s7Before.connection_id ||
        sl651Final.connection_id !== sl651Before.connection_id)
        throw new Error('unrelated protocol links changed during device lifecycle');

    console.log(
        JSON.stringify({
            deviceCode,
            finalVersion: version,
            affectedModbusConnection: modbusBefore.connection_id,
            preservedS7Connection: s7Before.connection_id,
            preservedSl651Connection: sl651Before.connection_id,
            lifecycle: ['create:TCP', 'update:RTU', 'disable', 'delete'],
            concurrentLifecycle: { devices: batchCodes.length, phases: ['create', 'disable', 'delete'] },
        }),
    );
} finally {
    for (const timer of timers) clearInterval(timer);
    for (const socket of sockets) socket.destroy();
    if (createdDeviceId) {
        try {
            await api('DELETE', `/api/device/${createdDeviceId}`);
        } catch {
            // Preserve the original failure; the test row is clearly named for manual cleanup.
        }
    }
    for (const id of batchDeviceIds) {
        try {
            await api('DELETE', `/api/device/${id}`);
        } catch {
            // Preserve the original failure while still attempting bounded fixture cleanup.
        }
    }
    await redis.close();
}
