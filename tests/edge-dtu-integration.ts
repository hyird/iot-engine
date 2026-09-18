import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { apiBase, databaseUrl, redisUrl } from './architecture-fixture';

const db = new Bun.SQL(databaseUrl), redis = new Bun.RedisClient(redisUrl);
const node = crypto.randomUUID(), channel = crypto.randomUUID();
const platform = '00000000-0000-7000-8000-000000000001';
const admin = '00000000-0000-7000-8000-000000000002';
const bytes = (id: string) => Buffer.from(id.replaceAll('-', ''), 'hex');
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const unsigned = `${encode({alg:'HS256',typ:'JWT'})}.${encode({iss:'iot-engine',aud:'iot-engine-web',sub:admin,user_id:admin,username:'admin',token_type:'access',iat:now,exp:now+3600})}`;
const token = `${unsigned}.${createHmac('sha256','architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;
function integer(input: bigint | number) {
    let value = BigInt(input); const output: number[] = [];
    do {let byte = Number(value & 127n); value >>= 7n; if (value) byte |= 128; output.push(byte);} while (value);
    return Buffer.from(output);
}
function field(tag: number, value: bigint | number | string | Buffer) {
    if (typeof value === 'bigint' || typeof value === 'number') return Buffer.concat([integer(tag * 8), integer(value)]);
    const payload = typeof value === 'string' ? Buffer.from(value) : value;
    return Buffer.concat([integer(tag * 8 + 2), integer(payload.length), payload]);
}
function decode(data: Buffer) {
    let position = 0; const fields = new Map<number, bigint | Buffer>();
    const read = () => {let value = 0n, shift = 0n; for (;;) {const byte = data[position++]; assert(byte !== undefined); value |= BigInt(byte & 127) << shift; if (!(byte & 128)) return value; shift += 7n;}};
    while (position < data.length) {const tag = Number(read()); if (!(tag & 7)) fields.set(tag >> 3, read()); else {assert.equal(tag & 7, 2); const size = Number(read()); fields.set(tag >> 3, data.subarray(position, position + size)); position += size;}}
    return fields;
}
async function until(check: () => Promise<boolean>, reason: string) {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {if (await check()) return; await Bun.sleep(50);}
    throw Error(reason);
}
async function api(method: string, suffix = '', body?: unknown, expected = 200) {
    const response = await fetch(`${apiBase}/v1/edge/${node}/dtu${suffix}`, {method,
        headers: {Authorization:`Bearer ${token}`, 'Content-Type':'application/json'},
        body: body ? JSON.stringify(body) : undefined, signal: AbortSignal.timeout(20000)});
    const text = await response.text(); assert.equal(response.status, expected, text);
    return JSON.parse(text);
}
let socket: WebSocket | undefined, epoch = 0n, sequence = 1n;
const items: Map<number, bigint | Buffer>[] = [];
const envelope = (tag: number, payload: Buffer) => Buffer.concat([field(1,6),field(2,bytes(crypto.randomUUID())),field(3,bytes(node)),field(4,bytes(platform)),field(5,epoch),field(6,Date.now()),field(8,sequence++),field(tag,payload)]);
const stem = `99${String(Math.floor(Math.random() * 1e12)).padStart(12,'0')}`;
let checksum = 0;
for (let i = 0; i < 14; ++i) {let digit = Number(stem[i]) * (i % 2 ? 2 : 1); if (digit > 9) digit -= 9; checksum += digit;}
const imei = stem + String((10 - checksum % 10) % 10);
const config = {channelId:channel,name:'独立透传测试',enabled:true,southMode:'tcp_server',southHost:'127.0.0.1',southPort:35001,
    northHost:'127.0.0.1',northPort:35002,maxClients:3,queueBytes:4096,serialFrameMs:0,debugEnabled:true,registrationHex:'0044455600',heartbeatHex:'004842FF',heartbeatIntervalSec:30};
try {
    await db`INSERT INTO edge_node(id,platform_id,imei,name,enrollment_status) VALUES(${node},${platform},${imei},'DTU 集成测试','approved')`;
    await redis.send('SET',[`iot:edge:auth:${imei}`,`${node}|approved`]);
    assert.equal((await api('GET')).data.supported,false);
    await api('PUT','',config,409);
    socket = new WebSocket(apiBase.replace('http:','ws:') + '/edge/v1/connect');
    socket.binaryType = 'arraybuffer';
    await new Promise<void>((resolve,reject) => {
        const timer = setTimeout(() => reject(Error('DTU hello timeout')),10000);
        socket!.onopen = () => socket!.send(envelope(20,Buffer.concat([field(1,imei),field(2,'DTU fixture'),field(3,'0.4.0'),field(23,1)])));
        socket!.onerror = () => reject(Error('DTU WebSocket failed'));
        socket!.onmessage = (event) => {
            const message = decode(Buffer.from(event.data as ArrayBuffer));
            if (message.has(21)) {epoch = message.get(5) as bigint; clearTimeout(timer); resolve();}
            if (message.has(80)) socket!.send(envelope(81,message.get(80) as Buffer));
            if (message.has(31)) items.push(decode(message.get(31) as Buffer));
            if (message.has(32)) socket!.send(envelope(33,message.get(32) as Buffer));
        };
    });
    socket.send(envelope(26,Buffer.concat([field(7,Buffer.from([1,2,3])),field(9,1)])));
    await until(async () => (await api('GET')).data.supported, 'DTU capability not projected');
    await api('PUT','',config);
    await until(async () => items.some((item) => item.has(18)), 'DTU item not delivered');
    const dtu = decode(items.find((item) => item.has(18))!.get(18) as Buffer);
    assert.equal(dtu.get(10),3n);
    assert.deepEqual(dtu.get(12),Buffer.from(config.registrationHex,'hex'));
    assert.equal(dtu.get(15),1n);
    assert.deepEqual(dtu.get(16),Buffer.from(config.heartbeatHex,'hex'));
    assert.equal(dtu.get(17),30n);
    await api('PUT','',{...config,maxClients:17},400);
    await api('PUT','',{...config,registrationHex:'XYZ'},400);
    await api('PUT','',{...config,heartbeatHex:'XYZ'},400);
    await api('PUT','',{...config,heartbeatHex:''},400);
    await api('PUT','',{...config,heartbeatIntervalSec:86401},400);
    await api('PUT','',{...config,heartbeatIntervalSec:-1},400);
    await api('PUT','',{...config,northHost:'tcp://127.0.0.1'},400);
    await api('PUT','',{...config,channelId:crypto.randomUUID()},409);
    assert.equal((await api('GET')).data.channels.length,1);
    const before = (await db`SELECT count(*)::int AS count FROM device_data`)[0].count;
    const trace = Buffer.concat([field(1,1),field(2,'south-rx'),field(3,2),field(4,Buffer.from('00FF','hex')),field(5,2),field(6,12345)]);
    socket.send(envelope(88,Buffer.concat([field(1,bytes(channel)),field(2,'connected'),field(3,'listening'),field(4,3),field(5,123),field(9,trace)])));
    await until(async () => (await api('GET')).data.channels[0].status.upstreamBytes === '123','DTU status not projected');
    const status = (await api('GET')).data.channels[0].status;
    assert.equal(status.traces[0].direction,'south-rx');
    assert.equal(status.traces[0].payload,Buffer.from('00FF','hex').toString('base64'));
    assert.equal((await db`SELECT count(*)::int AS count FROM device_data`)[0].count,before,'DTU debugging entered telemetry storage');
    assert.equal(Number(await redis.send('EXISTS',[`iot:debug:v4:link:${channel}`])),0,'DTU debugging entered acquisition packet log');
    await api('PUT','',{...config,debugEnabled:false,heartbeatIntervalSec:0});
    await until(async () => items.some((item) => item.has(18) && !decode(item.get(18) as Buffer).has(15)),'debug disable not delivered');
    const disabled = decode(items.find((item) => item.has(18) && !decode(item.get(18) as Buffer).has(15))!.get(18) as Buffer);
    assert.equal(disabled.has(17),false,'heartbeat disable not delivered');
    socket.send(envelope(88,Buffer.concat([field(1,bytes(channel)),field(2,'connected'),field(5,124),field(9,trace)])));
    await until(async () => (await api('GET')).data.channels[0].status.upstreamBytes === '124','disabled debug status not projected');
    assert.equal((await api('GET')).data.channels[0].status.traces,undefined,'late DTU debug copy bypassed disabled switch');
    await api('DELETE',`/${channel}`);
    assert.equal((await api('GET')).data.channels.length,0);
    console.log('PASS DTU capability gate, validated HTTP configuration, WS snapshot, registration, multi-client limit, isolated debug projection and deletion');
} finally {
    socket?.close();
    await redis.send('DEL',[`iot:edge:auth:${imei}`,`iot:edge:metadata:${node}`]);
    await db`DELETE FROM edge_dtu WHERE node_id=${node}`;
    await db.close(); redis.close();
}
