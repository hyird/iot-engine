import net from 'node:net';

const fixture = await Bun.file('build/stress-fixture.json').json();
const deviceCount = Number(fixture.deviceCount);
const durationMs = Number(Bun.env.STRESS_DURATION_MS ?? '60000');
const host = '127.0.0.1';
const redisUrl = Bun.env.REDIS_PASSWORD
    ? `redis://:${encodeURIComponent(Bun.env.REDIS_PASSWORD)}@${Bun.env.REDIS_HOST ?? host}:${Bun.env.REDIS_PORT ?? '6379'}/${Bun.env.REDIS_DATABASE ?? '0'}`
    : `redis://${Bun.env.REDIS_HOST ?? host}:${Bun.env.REDIS_PORT ?? '6379'}/${Bun.env.REDIS_DATABASE ?? '0'}`;
const redis = new Bun.RedisClient(redisUrl);
const sockets = new Set<net.Socket>();
const servers: net.Server[] = [];
const timers = new Set<Timer>();
const commandIssues = new Set<Promise<void>>();
const commandCoverage = new Map<string, string>();
const serverDevices = new Map<string, net.Socket>();
let stopped = false;
let ingressFrames = 0;
let egressFrames = 0;
let reconnects = 0;
let commands = 0;
let discoveryCommands = 0;
let socketErrors = 0;
let maxEventLoopLagMs = 0;
let transaction = 1;
let commandCursor = 0;
let peakRouteCount = 0;
const startedAtMs = Date.now();

const pad = (value: number, width: number) => value.toString().padStart(width, '0');

let uuidTimestamp = 0n;
let uuidSequence = 0n;
function uuidv7() {
    let timestamp = BigInt(Date.now());
    if (timestamp <= uuidTimestamp) timestamp = uuidTimestamp;
    else {
        uuidTimestamp = timestamp;
        const randomHex = [...crypto.getRandomValues(new Uint8Array(10))]
            .map((byte) => byte.toString(16).padStart(2, '0'))
            .join('');
        uuidSequence = BigInt(`0x${randomHex}`) & ((1n << 74n) - 1n);
    }
    uuidSequence = (uuidSequence + 1n) & ((1n << 74n) - 1n);
    const bytes = new Uint8Array(16);
    let time = timestamp;
    for (let index = 5; index >= 0; index--) {
        bytes[index] = Number(time & 0xffn);
        time >>= 8n;
    }
    let random = uuidSequence;
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

function crc16(bytes: Uint8Array) {
    let crc = 0xffff;
    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
    }
    return crc;
}

function withCrc(bytes: number[]) {
    const result = Buffer.from(bytes);
    const crc = crc16(result);
    return Buffer.concat([result, Buffer.from([crc & 0xff, crc >> 8])]);
}

const modbusFunctionCodes = [1, 2, 3, 4, 5, 6, 15, 16] as const;

function modbusRequest(
    index: number,
    tcp: boolean,
    functionCode: (typeof modbusFunctionCodes)[number],
    tx = transaction++ & 0xffff,
) {
    const unit = index + 1;
    const pdu =
        functionCode >= 1 && functionCode <= 4
            ? [unit, functionCode, 0, 0, 0, functionCode <= 2 ? 8 : 1]
            : functionCode === 5
              ? [unit, functionCode, 0, 0, 0xff, 0]
              : functionCode === 6
                ? [unit, functionCode, 0, 0, 0x12, 0x34]
                : functionCode === 15
                  ? [unit, functionCode, 0, 0, 0, 8, 1, 0xaa]
                  : [unit, functionCode, 0, 0, 0, 2, 4, 0x12, 0x34, 0x56, 0x78];
    if (!tcp) return withCrc(pdu);
    return Buffer.from([tx >> 8, tx & 0xff, 0, 0, pdu.length >> 8, pdu.length & 0xff, ...pdu]);
}

function modbusResponse(request: Buffer, tcp: boolean, forcedUnit?: number) {
    const unitOffset = tcp ? 6 : 0;
    const functionOffset = unitOffset + 1;
    const unit = forcedUnit ?? request[unitOffset];
    const fn = request[functionOffset];
    let pdu: number[];
    if (fn >= 1 && fn <= 4) {
        const quantity = request.readUInt16BE(functionOffset + 3);
        const byteCount = fn <= 2 ? Math.ceil(quantity / 8) : quantity * 2;
        const pattern =
            fn <= 2
                ? [0xaa]
                : [0x12, 0x34, 0x56, 0x78, 0x3f, 0xc0, 0, 0, 0x01, 0x23, 0x45, 0x67];
        const data = Array.from({ length: byteCount }, (_, offset) => pattern[offset % pattern.length]);
        pdu = [unit, fn, byteCount, ...data];
    } else {
        pdu = [unit, fn, ...request.subarray(functionOffset + 1, functionOffset + 5)];
    }
    if (!tcp) return withCrc(pdu);
    return Buffer.from([
        request[0],
        request[1],
        0,
        0,
        pdu.length >> 8,
        pdu.length & 0xff,
        ...pdu,
    ]);
}

function modbusRequestLength(buffer: Buffer, tcp: boolean) {
    if (tcp) {
        if (buffer.length < 6) return 0;
        return 6 + buffer.readUInt16BE(4);
    }
    if (buffer.length < 2) return 0;
    const fn = buffer[1];
    if (fn === 15 || fn === 16) {
        if (buffer.length < 7) return 0;
        return 9 + buffer[6];
    }
    return 8;
}

function attachModbus(socket: net.Socket, index: number, allowMany = false) {
    let buffered = Buffer.alloc(0);
    socket.on('data', (chunk) => {
        buffered = Buffer.concat([buffered, chunk]);
        const tcp = index % 2 === 0;
        while (buffered.length >= (tcp ? 6 : 2)) {
            const length = modbusRequestLength(buffered, tcp);
            if (length === 0) break;
            if (length < 8 || length > 260 || buffered.length < length) break;
            const request = buffered.subarray(0, length);
            buffered = buffered.subarray(length);
            if (allowMany && (request[tcp ? 6 : 0] === 0 || request[tcp ? 6 : 0] === 1)) {
                for (let unit = 1; unit <= 5; unit++) {
                    socket.write(modbusResponse(request, tcp, unit));
                    ingressFrames++;
                }
            } else {
                socket.write(modbusResponse(request, tcp));
                ingressFrames++;
            }
            egressFrames++;
        }
    });
}

const cotpConfirm = Buffer.from([3, 0, 0, 7, 2, 0xd0, 0]);
function setupResponse(reference: number) {
    return Buffer.from([3, 0, 0, 0x1b, 2, 0xf0, 0x80, 0x32, 3, 0, 0, reference >> 8, reference & 0xff, 0, 8, 0, 0, 0, 0, 0xf0, 0, 0, 1, 0, 1, 1, 0xe0]);
}
function s7Read(index: number, reference = transaction++ & 0xffff) {
    return Buffer.from([3, 0, 0, 0x1f, 2, 0xf0, 0x80, 0x32, 1, 0, 0, reference >> 8, reference & 0xff, 0, 0x0e, 0, 0, 4, 1, 0x12, 0x0a, 0x10, 2, 0, 2, 0, 1, 0x84, 0, 0, 0]);
}
function s7Write(reference = transaction++ & 0xffff) {
    return Buffer.from([3, 0, 0, 19, 2, 0xf0, 0x80, 0x32, 1, 0, 0, reference >> 8, reference & 0xff, 0, 2, 0, 0, 5, 1]);
}
function s7ReadResponse(request: Buffer) {
    const reference = request.readUInt16BE(11);
    const count = request[18];
    const items: number[] = [];
    for (let index = 0; index < count; index++) {
        const offset = 19 + index * 12;
        const wordLength = request[offset + 3];
        const amount = request.readUInt16BE(offset + 4);
        const byteLength = wordLength === 0x1c || wordLength === 0x1d ? amount * 2 : amount;
        const pattern = [0x12, 0x34, 0x56, 0x78, 0x3f, 0xc0, 0, 0, 0x41, 0x42, 0x43, 0];
        items.push(0xff, 0x04, (byteLength * 8) >> 8, (byteLength * 8) & 0xff);
        for (let byte = 0; byte < byteLength; byte++) items.push(pattern[byte % pattern.length]);
        if (byteLength % 2 === 1 && index + 1 < count) items.push(0);
    }
    const parameter = [0x04, count];
    const totalLength = 19 + parameter.length + items.length;
    return Buffer.from([
        3,
        0,
        totalLength >> 8,
        totalLength & 0xff,
        2,
        0xf0,
        0x80,
        0x32,
        3,
        0,
        0,
        reference >> 8,
        reference & 0xff,
        0,
        parameter.length,
        items.length >> 8,
        items.length & 0xff,
        0,
        0,
        ...parameter,
        ...items,
    ]);
}
function s7WriteResponse(reference: number) {
    return Buffer.from([3, 0, 0, 21, 2, 0xf0, 0x80, 0x32, 3, 0, 0, reference >> 8, reference & 0xff, 0, 2, 0, 0, 0, 0, 5, 1]);
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
            if (frame[5] === 0xe0) socket.write(cotpConfirm);
            else if (frame[17] === 0xf0) socket.write(setupResponse(frame.readUInt16BE(11)));
            else if (frame[17] === 4) socket.write(s7ReadResponse(frame));
            else if (frame[17] === 5) socket.write(s7WriteResponse(frame.readUInt16BE(11)));
            else continue;
            ingressFrames++;
            egressFrames++;
        }
    });
}

function sl651Frame(index: number, fn = 0x30, downstream = false) {
    const code = pad(index + 1, 10);
    const codeBytes = Array.from({ length: 5 }, (_, offset) => Number.parseInt(code.slice(offset * 2, offset * 2 + 2), 16));
    const functionIndex = fn >= 0x30 && fn <= 0x4f ? fn - 0x30 : -1;
    const encodingIndex = functionIndex >= 0 ? functionIndex % 5 : -1;
    const value =
        encodingIndex === 0
            ? [0x12, 0x34]
            : encodingIndex === 1
              ? [0x24, 0x01, 0x02, 0x03, 0x04, 0x05]
              : encodingIndex >= 2
                ? [0x00, 0xab, 0xff]
                : [];
    const body = functionIndex >= 0 ? [0x39, functionIndex, ...value] : [];
    const length = body.length | (downstream ? 0x8000 : 0);
    const address = downstream ? [...codeBytes, 1] : [1, ...codeBytes];
    const frame = Buffer.from([
        0x7e, 0x7e, ...address, 0, 0, fn, length >> 8, length & 0xff, 2, ...body, 3,
    ]);
    const crc = crc16(frame);
    return Buffer.concat([frame, Buffer.from([crc >> 8, crc & 0xff])]);
}

function track(socket: net.Socket) {
    sockets.add(socket);
    socket.setNoDelay(true);
    socket.on('error', () => socketErrors++);
    socket.on('close', () => sockets.delete(socket));
    return socket;
}

function createTargetServer(protocol: 'Modbus' | 'S7', index: number) {
    const port = (protocol === 'Modbus' ? 16000 : 16100) + index;
    const server = net.createServer((socket) => {
        track(socket);
        if (protocol === 'Modbus') attachModbus(socket, index);
        else attachS7(socket);
    });
    server.listen(port, host);
    servers.push(server);
}

function reconnectServerDevice(protocol: 'Modbus' | 'S7' | 'SL651', index: number, cycle = 0) {
    if (stopped) return;
    const port =
        protocol === 'Modbus'
            ? fixture.serverPorts.modbus
            : protocol === 'S7'
              ? fixture.serverPorts.s7
              : fixture.serverPorts.sl651;
    const key = `${protocol}:${index}`;
    const socket = track(net.createConnection({ host, port }));
    serverDevices.set(key, socket);
    socket.on('connect', () => {
        reconnects++;
        const delayedRegistration =
            protocol !== 'SL651' && index % 10 === 0 && cycle % 2 === 0;
        if (!delayedRegistration) {
            if (protocol === 'Modbus') socket.write(Buffer.from(`M${pad(index, 3)}`));
            else if (protocol === 'S7') socket.write(Buffer.from(`S${pad(index, 3)}`));
            else socket.write(sl651Frame(index, 0x30 + ((index + cycle) % 0x20)));
        }
        if (protocol === 'Modbus') attachModbus(socket, index, index === 0);
        else if (protocol === 'S7') {
            attachS7(socket);
            socket.on('data', (data) => {
                if (delayedRegistration && data.includes(Buffer.from('S7PROBE'))) {
                    socket.write(Buffer.from(`S${pad(index, 3)}`));
                    ingressFrames++;
                }
            });
        } else {
            let buffer = Buffer.alloc(0);
            socket.on('data', (chunk) => {
                buffer = Buffer.concat([buffer, chunk]);
                if (buffer.includes(Buffer.from([0x7e, 0x7e]))) {
                    socket.write(sl651Frame(index, 0xe1));
                    ingressFrames++;
                    buffer = Buffer.alloc(0);
                }
            });
        }
        let sl651Sequence = cycle;
        const heartbeat = setInterval(() => {
            if (socket.destroyed) return;
            if (protocol === 'Modbus') socket.write(Buffer.from(`HM${pad(index, 3)}`));
            else if (protocol === 'S7') socket.write(Buffer.from(`HS${pad(index, 3)}`));
            else
                socket.write(
                    sl651Frame(index, 0x30 + ((index + sl651Sequence++) % 0x20)),
                );
            ingressFrames++;
        }, 1000);
        timers.add(heartbeat);
        socket.once('close', () => {
            clearInterval(heartbeat);
            timers.delete(heartbeat);
        });
    });
    socket.on('close', () => {
        if (stopped) return;
        const timer = setTimeout(() => {
            timers.delete(timer);
            reconnectServerDevice(protocol, index, cycle + 1);
        }, 100 + (index % 10) * 10);
        timers.add(timer);
    });
}

async function waitListening() {
    await Promise.all(servers.map((server) => new Promise<void>((resolve) => (server.listening ? resolve() : server.once('listening', resolve)))));
}

async function hash(key: string) {
    const raw = (await redis.send('HGETALL', [key])) as Record<string, string> | string[];
    if (!Array.isArray(raw)) return raw;
    return Object.fromEntries(
        Array.from({ length: raw.length / 2 }, (_, index) => [raw[index * 2], raw[index * 2 + 1]]),
    );
}

async function scan(pattern: string) {
    let cursor = '0';
    const result: string[] = [];
    do {
        const page = (await redis.send('SCAN', [cursor, 'MATCH', pattern, 'COUNT', '500'])) as [string, string[]];
        cursor = page[0];
        result.push(...page[1]);
    } while (cursor !== '0');
    return result;
}

async function onlineRouteCount() {
    const keys = await scan('iot:runtime:device:*');
    const connections = await Promise.all(
        keys.map((key) => redis.send('HGET', [key, 'connection_id'])),
    );
    return connections.filter(Boolean).length;
}

async function waitRuntimeReady() {
    const linkIds = Object.values(fixture.links) as string[];
    const deadline = Date.now() + 120_000;
    while (Date.now() < deadline) {
        const counts = await Promise.all(
            linkIds.map(async (linkId) => (await scan(`iot:runtime:link:${linkId}:worker:*`)).length),
        );
        if (counts.every((count) => count > 0)) return counts;
        await Bun.sleep(100);
    }
    const counts = await Promise.all(
        linkIds.map(async (linkId) => (await scan(`iot:runtime:link:${linkId}:worker:*`)).length),
    );
    throw new Error(`collector runtime not ready: ${JSON.stringify(counts)}`);
}

async function commandBacklog() {
    const keys = await scan('iot:channel:command:worker:*');
    const depths = await Promise.all(keys.map((key) => redis.send('XLEN', [key])));
    return depths.reduce((total, depth) => total + Number(depth), 0);
}

async function streamEntriesAdded(key: string) {
    try {
        const info = (await redis.send('XINFO', ['STREAM', key])) as Record<
            string,
            string | number
        >;
        return Number(info['entries-added'] ?? 0);
    } catch (error) {
        if (String(error).includes('no such key')) {
            return 0;
        }
        throw error;
    }
}

async function partitionStreamDepth(prefix: string) {
    const keys = await scan(`${prefix}:worker:*`);
    const depths = await Promise.all(keys.map((key) => redis.send('XLEN', [key])));
    return depths.reduce((total, depth) => total + Number(depth), 0);
}

async function partitionStreamDepths(prefix: string) {
    const keys = await scan(`${prefix}:worker:*`);
    return Promise.all(keys.map(async (key) => ({ key, depth: Number(await redis.send('XLEN', [key])) })));
}

async function partitionEntriesAdded(prefix: string) {
    const keys = await scan(`${prefix}:worker:*`);
    const entries = await Promise.all(keys.map(streamEntriesAdded));
    return entries.reduce((total, count) => total + count, 0);
}

async function commandResultStatsSince(timestampMs: number) {
    const keys = await scan('iot:state:command:*');
    let success = 0;
    let failure = 0;
    const reasons = new Map<string, number>();
    const covered = new Set<string>();
    let sampled = 0;
    for (const key of keys) {
        const value = await hash(key);
        if (Number(value.completed_at_ms ?? 0) < timestampMs) continue;
        sampled++;
        if (value.success === '1') {
            success++;
            const label = commandCoverage.get(value.command_id);
            if (label) covered.add(label);
        } else failure++;
        const reason = value.reason || 'none';
        reasons.set(reason, (reasons.get(reason) ?? 0) + 1);
    }
    return {
        sampled,
        success,
        failure,
        modbusCommandFrameInvalid: reasons.get('modbus_command_frame_invalid') ?? 0,
        covered: [...covered].sort(),
        reasons: Object.fromEntries([...reasons].sort((left, right) => right[1] - left[1]).slice(0, 10)),
    };
}

async function enqueue(fields: Record<string, string>, worker: string, high = true) {
    const stream = `iot:channel:command:worker:${worker}:${high ? 'high' : 'normal'}`;
    const args = [stream, 'MAXLEN', '~', '10000', '*'];
    for (const [key, value] of Object.entries(fields)) args.push(key, value);
    await redis.send('XADD', args);
    commands++;
}

function taskFields(id: string, protocol: string, linkId: string, deviceId: string, deviceCode: string, connectionId: string, payload: Buffer, kind: string, readback = Buffer.alloc(0), expected = Buffer.alloc(0), protocolTransport = 'TCP', sessionEpoch = '') {
    return {
        message_id: id, causation_id: '', group_key: `device:${deviceCode}`, protocol,
        transport: protocolTransport, kind, link_id: linkId, device_id: deviceId, device_code: deviceCode,
        connection_id: connectionId, payload_hex: payload.toString('hex').toUpperCase(),
        readback_payload_hex: readback.toString('hex').toUpperCase(),
        expected_readback_hex: expected.toString('hex').toUpperCase(), expected_value: '',
        expects_response: '1', response_timeout_ms: '3000', created_at_ms: String(Date.now()),
        attempt: '1', max_attempts: '3', session_epoch: sessionEpoch,
    };
}

async function enqueueDiscovery(protocol: 'Modbus' | 'S7', payload: Buffer, transport = 'TCP') {
    const linkId = protocol === 'Modbus' ? fixture.links.modbusServer : fixture.links.s7Server;
    const stateKeys = await scan(`iot:runtime:link:${linkId}:worker:*`);
    const workers = new Set(stateKeys.map((key) => key.split(':').at(-1) as string));
    for (const worker of workers) {
        await enqueue(taskFields(uuidv7(), protocol, linkId, '', '', '', payload, 'discovery', Buffer.alloc(0), Buffer.alloc(0), transport), worker);
        discoveryCommands++;
    }
}

for (let index = 0; index < deviceCount; index++) {
    createTargetServer('Modbus', index);
    createTargetServer('S7', index);
}
await waitListening();
console.log(`target servers ready: ${servers.length}`);
const readyLinkStates = await waitRuntimeReady();
console.log(`collector runtime ready: ${readyLinkStates.join(',')}`);
const initialCommandBacklog = await commandBacklog();
if (initialCommandBacklog !== 0)
    throw new Error(`command streams were not empty before stress: ${initialCommandBacklog}`);

const baselineResultEntries = await partitionEntriesAdded('iot:channel:command:result');

for (let index = 0; index < deviceCount; index++) {
    reconnectServerDevice('Modbus', index);
    reconnectServerDevice('S7', index);
    reconnectServerDevice('SL651', index);
}

async function issueDeviceCommands(index: number, attempt: number) {
    try {
        const modbusCode = `MS${pad(index, 3)}`;
        const modbusRoute = await hash(`iot:runtime:device:${modbusCode}`);
        if (modbusRoute.connection_id) {
            const tcp = index % 2 === 0;
            const functionCode =
                modbusFunctionCodes[(Math.floor(index / 2) + attempt) % modbusFunctionCodes.length];
            const write = functionCode === 5 || functionCode === 6 || functionCode === 15 || functionCode === 16;
            const payload = modbusRequest(index, tcp, functionCode);
            const readFunction = functionCode === 5 || functionCode === 15 ? 1 : 3;
            const readback = write ? modbusRequest(index, tcp, readFunction) : Buffer.alloc(0);
            const expected = write
                ? readFunction === 1
                    ? Buffer.from([0xaa])
                    : Buffer.from([0x12, 0x34])
                : Buffer.alloc(0);
            const commandId = uuidv7();
            commandCoverage.set(
                commandId,
                `Modbus:${tcp ? 'TCP' : 'RTU'}:FC${functionCode.toString(16).toUpperCase().padStart(2, '0')}`,
            );
            await enqueue(taskFields(commandId, 'Modbus', fixture.links.modbusServer, modbusRoute.device_id, modbusCode, modbusRoute.connection_id, payload, write ? 'write' : 'read', readback, expected, tcp ? 'TCP' : 'RTU', modbusRoute.session_epoch), modbusRoute.worker_id);
        }
        const s7Code = `SS${pad(index, 3)}`;
        const s7Route = await hash(`iot:runtime:device:${s7Code}`);
        if (s7Route.connection_id) {
            const write = attempt % 2 === 0;
            const payload = write ? s7Write() : s7Read(index);
            const readback = write ? s7Read(index) : Buffer.alloc(0);
            const commandId = uuidv7();
            commandCoverage.set(commandId, `S7:FC${write ? '05' : '04'}`);
            await enqueue(taskFields(commandId, 'S7', fixture.links.s7Server, s7Route.device_id, s7Code, s7Route.connection_id, payload, write ? 'write' : 'read', readback, write ? Buffer.from([0x12, 0x34]) : Buffer.alloc(0), 'TCP', s7Route.session_epoch), s7Route.worker_id);
        }
        const slCode = pad(index + 1, 10);
        const slRoute = await hash(`iot:runtime:device:${slCode}`);
        if (slRoute.connection_id) {
            const functionCode = 0x30 + (attempt % 0x20);
            const commandId = uuidv7();
            commandCoverage.set(
                commandId,
                `SL651:FC${functionCode.toString(16).toUpperCase().padStart(2, '0')}`,
            );
            await enqueue(taskFields(commandId, 'SL651', fixture.links.sl651Server, slRoute.device_id, slCode, slRoute.connection_id, sl651Frame(index, functionCode, true), 'control', Buffer.alloc(0), Buffer.alloc(0), 'TCP', slRoute.session_epoch), slRoute.worker_id, false);
        }
    } catch {
        socketErrors++;
    }
}

const commandTimer = setInterval(() => {
    if (stopped || commandIssues.size >= 256) return;
    for (let burst = 0; burst < 4; burst++) {
        const attempt = commandCursor++;
        const operation = issueDeviceCommands(attempt % deviceCount, attempt).finally(() =>
            commandIssues.delete(operation),
        );
        commandIssues.add(operation);
    }
}, 25);
timers.add(commandTimer);

const discoveryTimer = setInterval(async () => {
    try {
        await enqueueDiscovery('Modbus', modbusRequest(0, true, 3), 'TCP');
        await enqueueDiscovery('Modbus', modbusRequest(0, false, 3), 'RTU');
        await enqueueDiscovery('S7', Buffer.from('S7PROBE'));
    } catch {
        socketErrors++;
    }
}, 5000);
timers.add(discoveryTimer);

const lifecycleTimer = setInterval(() => {
    for (let offset = 0; offset < 20; offset++) {
        const index = (reconnects + offset * 7) % deviceCount;
        const protocol = (['Modbus', 'S7', 'SL651'] as const)[offset % 3];
        serverDevices.get(`${protocol}:${index}`)?.destroy();
    }
    for (let offset = 0; offset < 10; offset++) {
        const candidates = [...sockets].filter((socket) => !socket.destroyed);
        candidates[(offset * 13 + reconnects) % Math.max(1, candidates.length)]?.destroy();
    }
}, 4000);
timers.add(lifecycleTimer);

let expectedTick = Date.now() + 100;
const lagTimer = setInterval(() => {
    const now = Date.now();
    maxEventLoopLagMs = Math.max(maxEventLoopLagMs, now - expectedTick);
    expectedTick = now + 100;
}, 100);
timers.add(lagTimer);

const metricsTimer = setInterval(async () => {
    const routeCount = await onlineRouteCount();
    peakRouteCount = Math.max(peakRouteCount, routeCount);
    const parsed = await partitionStreamDepth('iot:channel:packet:parsed');
    const results = await partitionStreamDepth('iot:channel:command:result');
    const linkStates = (await scan('iot:runtime:link:*:worker:*')).length;
    console.log(JSON.stringify({ routeCount, parsed, results, linkStates, ingressFrames, egressFrames, reconnects, commands, discoveryCommands, socketErrors, maxEventLoopLagMs }));
}, 5000);
timers.add(metricsTimer);

await Bun.sleep(durationMs);
stopped = true;
for (const timer of timers) clearInterval(timer);
await Promise.allSettled([...commandIssues]);
const commandSettleDeadline = Date.now() + 10000;
let remainingCommandBacklog = await commandBacklog();
while (remainingCommandBacklog > 0 && Date.now() < commandSettleDeadline) {
    await Bun.sleep(100);
    remainingCommandBacklog = await commandBacklog();
}
const liveRouteCount = await onlineRouteCount();
peakRouteCount = Math.max(peakRouteCount, liveRouteCount);
for (const socket of sockets) socket.destroy();
await Promise.all(servers.map((server) => new Promise<void>((resolve) => server.close(() => resolve()))));
const cleanupDeadline = Date.now() + 10000;
let remainingRouteCount = await onlineRouteCount();
while (remainingRouteCount > 0 && Date.now() < cleanupDeadline) {
    await Bun.sleep(100);
    remainingRouteCount = await onlineRouteCount();
}
const postDisconnectCommandDeadline = Date.now() + 10000;
remainingCommandBacklog = await commandBacklog();
while (remainingCommandBacklog > 0 && Date.now() < postDisconnectCommandDeadline) {
    await Bun.sleep(100);
    remainingCommandBacklog = await commandBacklog();
}
const resultDelta = Math.max(
    0,
    (await partitionEntriesAdded('iot:channel:command:result')) - baselineResultEntries,
);
const resultStats = await commandResultStatsSince(startedAtMs);
const parsedDepth = await partitionStreamDepth('iot:channel:packet:parsed');
const resultDepth = await partitionStreamDepth('iot:channel:command:result');
const deadLetterDepthByWorker = await partitionStreamDepths('iot:channel:dead-letter');
const deadLetterDepth = deadLetterDepthByWorker.reduce((total, item) => total + item.depth, 0);
const maxDeadLetterWorkerDepth = Math.max(0, ...deadLetterDepthByWorker.map((item) => item.depth));
const expectedLatestElementIds = [
    ...fixture.coverage.modbusElementIds,
    ...fixture.coverage.s7ElementIds,
    ...fixture.coverage.sl651TelemetryElementIds,
] as string[];
const latestSamples = (fixture.coverage.latestSamples as Array<{
    deviceCode: string;
    elementId?: string;
}>).filter(
    (sample): sample is { deviceCode: string; elementId: string } => !!sample.elementId,
);
const latestObservations = await Promise.all(
    latestSamples.map((sample) =>
        redis.send('HGET', [
            `iot:device:${sample.deviceCode}:latest`,
            sample.elementId,
        ]),
    ),
);
const latestUpdatedAt = latestObservations.map((raw) => {
    if (!raw) return 0;
    try {
        return Number((JSON.parse(String(raw)) as { updatedAt?: number }).updatedAt ?? 0);
    } catch {
        return 0;
    }
});
const missingLatestElementIds = latestSamples
    .filter((_, index) => latestUpdatedAt[index] < startedAtMs)
    .map((sample) => sample.elementId);
const stressDeviceIds = Object.values(fixture.devices).flat() as string[];
const deviceIdList = stressDeviceIds.map((id) => `'${id.replaceAll("'", "''")}'`).join(',');
const database = Bun.spawn([
    'docker', 'exec', '-e', `PGPASSWORD=${Bun.env.DB_PASSWORD ?? ''}`, 'timescaledb', 'psql', '-U',
    Bun.env.DB_USERNAME ?? 'postgres', '-d', Bun.env.DB_DATABASE ?? 'postgres', '-Atc',
    `WITH telemetry AS (
       SELECT data FROM device_data
       WHERE device_id IN (${deviceIdList})
         AND created_at >= to_timestamp(${startedAtMs} / 1000.0)
     ), elements AS (
       SELECT DISTINCT jsonb_object_keys(COALESCE(data->'values', '{}'::jsonb)) AS id
       FROM telemetry
     )
     SELECT (SELECT COUNT(*) FROM telemetry) || '|' || (SELECT COUNT(*) FROM elements)`,
], { stdout: 'pipe', stderr: 'inherit' });
const [telemetryRows, persistedElementCount] = (await new Response(database.stdout).text())
    .trim()
    .split('|')
    .map(Number);
if ((await database.exited) !== 0) throw new Error('telemetry count query failed');
await redis.close();

const expectedCommandCoverage = [
    ...(['TCP', 'RTU'] as const).flatMap((mode) =>
        modbusFunctionCodes.map(
            (functionCode) =>
                `Modbus:${mode}:FC${functionCode.toString(16).toUpperCase().padStart(2, '0')}`,
        ),
    ),
    'S7:FC04',
    'S7:FC05',
    ...(fixture.coverage.sl651FunctionCodes as string[]).map((code) => `SL651:FC${code}`),
];
const coveredCommands = new Set(resultStats.covered);
const missingCommandCoverage = expectedCommandCoverage.filter((item) => !coveredCommands.has(item));
const summary = { liveRouteCount, peakRouteCount, remainingRouteCount, remainingCommandBacklog, resultDelta, resultStats, parsedDepth, resultDepth, deadLetterDepth, maxDeadLetterWorkerDepth, telemetryRows, persistedElementCount, expectedElementCount: expectedLatestElementIds.length, missingLatestElementCount: missingLatestElementIds.length, missingCommandCoverage, ingressFrames, egressFrames, reconnects, commands, discoveryCommands, socketErrors, maxEventLoopLagMs };
console.log(JSON.stringify({ final: summary }));
if (peakRouteCount < Math.floor(deviceCount * 4.5)) throw new Error(`too few peak live routes: ${peakRouteCount}`);
const minimumResults = Math.min(100, Math.max(20, Math.floor(commands / 2)));
if (resultDelta < minimumResults) throw new Error(`too few command results: ${resultDelta}/${commands}`);
if (resultStats.success < 100) throw new Error(`too few successful command results: ${resultStats.success}`);
if (resultStats.modbusCommandFrameInvalid !== 0)
    throw new Error(`Modbus TCP/RTU mode was misclassified ${resultStats.modbusCommandFrameInvalid} times`);
if (telemetryRows < deviceCount * 5) throw new Error(`too few persisted telemetry rows: ${telemetryRows}`);
if (missingLatestElementIds.length !== 0)
    throw new Error(`Redis latest values missed ${missingLatestElementIds.length} configured elements`);
if (persistedElementCount < expectedLatestElementIds.length)
    throw new Error(`TimescaleDB persisted only ${persistedElementCount}/${expectedLatestElementIds.length} configured elements`);
if (missingCommandCoverage.length !== 0)
    throw new Error(`command coverage missing: ${missingCommandCoverage.join(', ')}`);
if (remainingCommandBacklog !== 0)
    throw new Error(`command streams retained ${remainingCommandBacklog} entries`);
if (parsedDepth > 1000) throw new Error(`parsed stream backlog is unbounded: ${parsedDepth}`);
if (resultDepth > 11000) throw new Error(`command result stream is unbounded: ${resultDepth}`);
if (maxDeadLetterWorkerDepth > 1100)
    throw new Error(`a worker dead-letter stream is unbounded: ${maxDeadLetterWorkerDepth}`);
if (remainingRouteCount !== 0) throw new Error(`stale device routes remained: ${remainingRouteCount}`);
if (maxEventLoopLagMs > 1000) throw new Error(`stress driver event loop stalled: ${maxEventLoopLagMs}ms`);
