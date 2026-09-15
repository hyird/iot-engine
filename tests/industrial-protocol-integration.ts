import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { apiBase, databaseUrl } from './architecture-fixture';

const db = new Bun.SQL(databaseUrl);
const admin = '00000000-0000-7000-8000-000000000002';
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const unsigned = `${encode({alg: 'HS256', typ: 'JWT'})}.${encode({iss: 'iot-engine', aud: 'iot-engine-web', sub: admin,
    user_id: admin, username: 'admin', token_type: 'access', iat: now, exp: now + 3600})}`;
const token = `${unsigned}.${createHmac('sha256', 'architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;
async function request(path: string, body: unknown, method = 'POST') {
    const response = await fetch(apiBase + path, {method, headers: {Authorization: `Bearer ${token}`, 'Content-Type': 'application/json'},
        body: JSON.stringify(body), signal: AbortSignal.timeout(20000)});
    const text = await response.text();
    assert.equal(response.status, 200, `${path}: ${text}`);
    return JSON.parse(text);
}
async function until(check: () => Promise<boolean>, reason: string) {
    const deadline = Date.now() + 25000;
    while (Date.now() < deadline) { if (await check()) return; await Bun.sleep(50); }
    throw Error(reason);
}
function meterFrame(request: Buffer, control: number, data: Buffer) {
    const frame = Buffer.alloc(12 + data.length);
    request.copy(frame, 0, 0, 8); frame[8] = control; frame[9] = data.length;
    for (let i = 0; i < data.length; ++i) frame[10 + i] = (data[i] + 0x33) & 255;
    frame[frame.length - 2] = frame.subarray(0, -2).reduce((sum, value) => (sum + value) & 255, 0);
    frame[frame.length - 1] = 0x16;
    return frame;
}
async function verify(protocol: 'MC' | 'FINS' | 'DLT645', version: string) {
    let stored = protocol === 'DLT645' ? Buffer.from([0, 0x42, 0, 0]) : Buffer.from(protocol === 'MC' ? [42, 0] : [0, 42]);
    let writes = 0;
    const server = Bun.listen<{buffer: Buffer}>({hostname: '127.0.0.1', port: 0, socket: {
        open(socket) { socket.data = {buffer: Buffer.alloc(0)}; },
        data(socket, data) {
            let buffer = Buffer.concat([socket.data.buffer, Buffer.from(data)]);
            for (;;) {
                if (protocol === 'DLT645') while (buffer[0] === 0xfe) buffer = buffer.subarray(1);
                const header = protocol === 'MC' ? version === '4E' ? 13 : 9 : protocol === 'FINS' ? 8 : 10;
                if (buffer.length < header) break;
                const length = protocol === 'MC' ? header + buffer.readUInt16LE(header - 2) : protocol === 'FINS' ? 8 + buffer.readUInt32BE(4) : 12 + buffer[9];
                if (buffer.length < length) break;
                const frame = buffer.subarray(0, length); buffer = buffer.subarray(length);
                let reply: Buffer;
                if (protocol === 'MC') {
                    const write = frame.readUInt16LE(header + 2) === 0x1401;
                    if (write) { stored = Buffer.from(frame.subarray(header + 12)); ++writes; }
                    reply = Buffer.alloc(header + 2 + (write ? 0 : stored.length));
                    frame.copy(reply, 0, 0, header); reply[0] = version === '4E' ? 0xd4 : 0xd0;
                    reply.writeUInt16LE(reply.length - header, header - 2);
                    if (!write) stored.copy(reply, header + 2);
                } else if (protocol === 'FINS') {
                    if (frame.readUInt32BE(8) === 0) {
                        reply = Buffer.from([70,73,78,83,0,0,0,16,0,0,0,1,0,0,0,0,0,0,0,10,0,0,0,20]);
                    } else {
                        assert.equal(frame[20], 20); assert.equal(frame[23], 10);
                        const write = frame[27] === 2;
                        if (write) { stored = Buffer.from(frame.subarray(34)); ++writes; }
                        reply = Buffer.alloc(30 + (write ? 0 : stored.length));
                        frame.copy(reply, 0, 0, 28); reply.writeUInt32BE(reply.length - 8, 4); reply[16] = 0xc0;
                        frame.copy(reply, 19, 22, 25); frame.copy(reply, 22, 19, 22);
                        if (!write) stored.copy(reply, 30);
                    }
                } else {
                    const legacy = version === '1997', identifierLength = legacy ? 2 : 4;
                    const data = Buffer.from(frame.subarray(10, -2).map((value) => (value - 0x33) & 255));
                    const write = frame[8] === (legacy ? 4 : 0x14);
                    if (write) { stored = Buffer.from(data.subarray(identifierLength + (legacy ? 4 : 8))); ++writes; }
                    reply = meterFrame(frame, frame[8] | 0x80, write ? Buffer.alloc(0) : Buffer.concat([data.subarray(0, identifierLength), stored]));
                }
                socket.write(reply.subarray(0, 3)); socket.write(reply.subarray(3));
            }
            socket.data.buffer = Buffer.from(buffer);
        }, error(_socket, error) { throw error; }, close() {},
    }});
    const name = `industrial-${protocol}-${crypto.randomUUID().slice(0, 8)}`, link = crypto.randomUUID(), target = crypto.randomUUID(), device = crypto.randomUUID();
    const point = {id: '00000000-0000-7000-8000-000000000111', name: '测试点', dataType: protocol === 'DLT645' ? 'BCD' : 'UINT16', writable: true,
        ...(protocol === 'DLT645' ? {identifier: version === '1997' ? '9010' : '00000000', length: 4, digits: 2} :
            {area: 'D', address: 100, bit: 0, byteOrder: protocol === 'MC' ? 'LITTLE_ENDIAN' : 'BIG_ENDIAN'})};
    const connection = protocol === 'MC' ? {frame: version} : protocol === 'FINS' ? {} :
        {version, wakeupBytes: 4, writePassword: '00123456', operatorCode: '00000001'};
    const config = {connection, points: [point], readInterval: 1, storagePolicy: 'report', commandFastReadDuration: 3, commandFastReadInterval: 1};
    try {
        await request('/v1/protocol/configs', {name, protocol, config});
        const [model] = await db`SELECT id FROM protocol_config WHERE name=${name}`;
        assert(model, 'API did not persist device type');
        const endpoint = {transport: 'tcp', mode: 'TCP Client', ip: '', port: 0,
            targets: [{id: target, name: 'simulator', ip: '127.0.0.1', port: server.port, status: 'enabled'}]};
        await db`INSERT INTO link(id,name,protocol,endpoint,status,execution,created_by) VALUES(${link},${link},${protocol},${endpoint}::jsonb,'enabled','collector',${admin})`;
        const params = {device_code: '000000000001', target_id: target, remote_control: true};
        await db`INSERT INTO device(id,name,link_id,protocol_config_id,protocol_params,created_by)
            VALUES(${device},${device},${link},${model.id},${params}::jsonb,${admin})`;
        await request(`/v1/device/${device}/debug`, {enabled: true}, 'PUT');
        await until(async () => (await db`SELECT 1 FROM device_data WHERE device_id=${device} AND data->'values'->'00000000-0000-7000-8000-000000000111'->>'value' IN ('42','42.00')`).length > 0,
            `${protocol} ${version} did not collect a persisted value`);
        const result = await request(`/v1/device/${device}/commands`, {idempotency_key: crypto.randomUUID(), elements: [{elementId: '00000000-0000-7000-8000-000000000111', value: protocol === 'DLT645' ? '13.25' : '13'}]});
        assert.equal(result.data.command_ids.length, 1);
        const commandId = result.data.command_ids[0];
        await until(async () => (await db`SELECT 1 FROM command_operation WHERE id=${commandId} AND status='SUCCEEDED'`).length > 0,
            `${protocol} ${version} write did not finish through readback`);
        assert.equal(writes, 1, 'one command wrote more than once');
        console.log(`PASS ${protocol} ${version}: API configuration, snapshot, TCP polling, persistence, write and readback`);
    } finally {
        await db`UPDATE device SET deleted_at=NOW() WHERE id=${device}`;
        await db`UPDATE link SET status='disabled',deleted_at=NOW() WHERE id=${link}`;
        server.stop(true);
    }
}
try {
    for (const [protocol, version] of [['MC','3E'],['MC','4E'],['FINS','TCP'],['DLT645','1997'],['DLT645','2007']] as const) await verify(protocol, version);
} finally { await db.close(); }
