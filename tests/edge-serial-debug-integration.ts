import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { apiBase, databaseUrl, redisUrl } from './architecture-fixture';
const db = new Bun.SQL(databaseUrl), redis = new Bun.RedisClient(redisUrl);
const platform = '00000000-0000-7000-8000-000000000001', admin = '00000000-0000-7000-8000-000000000002';
const bytes = (id: string) => Buffer.from(id.replaceAll('-', ''), 'hex');
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now()/1000);
const unsigned = `${encode({alg:'HS256',typ:'JWT'})}.${encode({iss:'iot-engine',aud:'iot-engine-web',sub:admin,user_id:admin,username:'admin',token_type:'access',iat:now,exp:now+3600})}`;
const token = `${unsigned}.${createHmac('sha256','architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;
function integer(input: bigint | number) {
    let n = BigInt(input); const output: number[] = [];
    do { let byte = Number(n & 127n); n >>= 7n; if (n) byte |= 128; output.push(byte); } while(n);
    return Buffer.from(output);
}
function field(tag: number, value: bigint | number | string | Buffer) {
    if (typeof value === 'bigint' || typeof value === 'number') return Buffer.concat([integer(tag*8),integer(value)]);
    const buffer = typeof value === 'string' ? Buffer.from(value) : value;
    return Buffer.concat([integer(tag*8+2),integer(buffer.length),buffer]);
}
function decode(data: Buffer) {
    const result = new Map<number, bigint | Buffer>(); let position = 0;
    const read = () => { let n = 0n, shift = 0n; for (;;) { const b = data[position++]; assert(b !== undefined); n |= BigInt(b&127)<<shift; if (!(b&128)) return n; shift+=7n; } };
    while (position < data.length) {
        const key = Number(read()), wire = key&7;
        if (!wire) result.set(key>>3,read());
        else if (wire === 2) { const n = Number(read()); result.set(key>>3,data.subarray(position,position+n)); position+=n; }
        else if (wire === 1 || wire === 5) {const n=wire===1?8:4;result.set(key>>3,data.subarray(position,position+n));position+=n;}
        else throw Error(`invalid wire ${wire}`);
    }
    return result;
}
async function until(check: () => Promise<boolean>, reason: string) {
    const end = Date.now()+25000;
    while (Date.now()<end) {if(await check())return;await Bun.sleep(50);} throw Error(reason);
}
async function request(path: string, body: unknown, method = 'POST') {
    const response = await fetch(apiBase+path,{method,headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json'},body:JSON.stringify(body)});
    const text = await response.text();assert.equal(response.status,200,`${path}: ${text}`);return JSON.parse(text);
}
function imei() {
    const base='99'+String(Math.floor(Math.random()*1e12)).padStart(12,'0');let sum=0;
    for(let i=0;i<14;i++){let n=Number(base[i])*(i%2?2:1);if(n>9)n-=9;sum+=n;}return base+String((10-sum%10)%10);
}
const node = crypto.randomUUID(), identity = imei();
let socket: WebSocket | undefined, browser: WebSocket | undefined;
try {
    await until(async () => (await fetch(apiBase + '/internal/health/ready')).status === 200, 'workers not ready');
    await db`INSERT INTO edge_node(id,platform_id,imei,enrollment_status) VALUES(${node},${platform},${identity},'approved')`;
    await redis.send('SET', [`iot:edge:auth:${identity}`, `${node}|approved`]);
    let sequence = 1n, epoch = 0n, eventSequence = 0;
    const commands: Map<number, bigint | Buffer>[] = [];
    const envelope = (tag: number, payload: Buffer) => Buffer.concat([
        field(1, 6), field(2, bytes(crypto.randomUUID())), field(3, bytes(node)),
        field(4, bytes(platform)), field(5, epoch), field(6, Date.now()), field(8, sequence++), field(tag, payload),
    ]);
    socket = new WebSocket(apiBase.replace('http:', 'ws:') + '/edge/v1/connect');
    socket.binaryType = 'arraybuffer';
    await new Promise<void>((resolve, reject) => {
        const timer = setTimeout(() => reject(Error('Hello timeout')), 10000);
        socket!.onopen = () => socket!.send(envelope(20, Buffer.concat([field(1, identity), field(2, 'fixture'), field(3, 'serial-test'), field(23, 1)])));
        socket!.onerror = () => reject(Error('node WebSocket error'));
        socket!.onmessage = event => {
            const message = decode(Buffer.from(event.data as ArrayBuffer));
            if (message.has(21)) { epoch = message.get(5) as bigint; clearTimeout(timer); resolve(); }
            if (message.has(80)) socket!.send(envelope(81, message.get(80) as Buffer));
            if (message.has(86)) commands.push(decode(message.get(86) as Buffer));
        };
    });
    socket.send(envelope(26, field(8, 1)));
    await until(async () => (await db`SELECT 1 FROM edge_node WHERE id=${node} AND capability->>'serialDebug'='true'`).length > 0, 'serial capability missing');
    await db`INSERT INTO edge_node_serial(node_id,path,available) VALUES(${node},'/dev/ttyS1',true)`;
    const ticket = (await request(`/v1/edge/${node}/serial-ticket`, {path: '/dev/ttyS1'})).data.ticket;
    const events: {kind: string; requestId?: number; hex?: string; manual?: boolean}[] = [];
    browser = new WebSocket(apiBase.replace('http:', 'ws:') + `/edge/v1/serial?ticket=${ticket}`);
    browser.onmessage = event => events.push(JSON.parse(String(event.data)));
    await until(async () => commands.length > 0, 'serial open not delivered');
    const open = commands[0];
    assert.equal((open.get(3) as Buffer).toString(), 'open');
    const id = open.get(1) as Buffer;
    const settings = Buffer.concat([field(1, '/dev/ttyS1'), field(2, 9600), field(3, 8), field(4, 1), field(5, 'none')]);
    const emit = (kind: string, requestId: number, manual: boolean, data = Buffer.alloc(0)) => socket!.send(envelope(87, Buffer.concat([
        field(1, id), field(2, requestId), field(3, kind), field(4, Number(manual)), field(5, data.length ? 'RX' : ''),
        field(6, data), field(7, Date.now()), field(9, ++eventSequence), field(11, settings),
    ])));
    emit('state', 1, false);
    await until(async () => events.some(e => e.kind === 'state'), 'open state not delivered');
    const replay = new WebSocket(apiBase.replace('http:', 'ws:') + `/edge/v1/serial?ticket=${ticket}`);
    await new Promise<void>((resolve, reject) => { const timer = setTimeout(() => reject(Error('ticket replay remained open')), 5000); replay.onclose = () => {clearTimeout(timer); resolve();}; });
    browser.send(JSON.stringify({action: 'manual', requestId: 2, baudRate: 9600, dataBits: 8, stopBits: 1, parity: 'none', rs485: false}));
    await until(async () => commands.some(c => c.get(2) === 2n), 'manual command not delivered');
    emit('state', 2, true);
    await until(async () => events.some(e => e.requestId === 2 && e.manual), 'manual state missing');
    browser.send(JSON.stringify({action: 'write', requestId: 3, hex: '00FF0A80'}));
    await until(async () => commands.some(c => c.get(2) === 3n), 'write not delivered');
    assert.equal((commands.find(c => c.get(2) === 3n)!.get(5) as Buffer).toString('hex'), '00ff0a80');
    emit('data', 0, true, Buffer.from([0, 255, 10, 128])); emit('sent', 3, true);
    await until(async () => events.some(e => e.kind === 'sent'), 'write acknowledgement missing');
    assert(events.some(e => e.hex === '00FF0A80'));
    browser.send(JSON.stringify({action: 'monitor', requestId: 4}));
    await until(async () => commands.some(c => c.get(2) === 4n), 'resume not delivered');
    emit('state', 4, false);
    await until(async () => events.some(e => e.requestId === 4 && !e.manual), 'resume acknowledgement missing');
    browser.close();
    await until(async () => commands.some(c => (c.get(3) as Buffer).toString() === 'close'), 'browser disconnect did not close serial session');
    socket.send(envelope(26, Buffer.alloc(0)));
    await until(async () => (await db`SELECT 1 FROM edge_node WHERE id=${node} AND capability->>'serialDebug'='false'`).length > 0, 'legacy capability not reset');
    const denied = await fetch(apiBase + `/v1/edge/${node}/serial-ticket`, {method: 'POST', headers: {Authorization: `Bearer ${token}`, 'Content-Type': 'application/json'}, body: JSON.stringify({path: '/dev/ttyS1'})});
    assert.equal(denied.status, 409);
    console.log('PASS real HTTP ticket, single-use replay rejection, Redis/WebSocket open/manual/binary/resume/close, legacy capability gate');
} finally {
    browser?.close(); socket?.close();
    await redis.send('DEL', [`iot:edge:auth:${identity}`]); redis.close(); await db.close();
}
