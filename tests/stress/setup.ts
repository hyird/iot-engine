const deviceCount = Number(Bun.env.STRESS_DEVICE_COUNT ?? '100');
const adminId = '00000000-0000-7000-8000-000000000002';

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

const quote = (value: string) => `'${value.replaceAll("'", "''")}'`;
const json = (value: unknown) => `${quote(JSON.stringify(value))}::jsonb`;
const pad = (value: number, width: number) => value.toString().padStart(width, '0');

const links = {
    modbusServer: uuidv7(),
    modbusClient: uuidv7(),
    s7Server: uuidv7(),
    s7Client: uuidv7(),
    sl651Server: uuidv7(),
};
const protocols = { Modbus: uuidv7(), S7: uuidv7(), SL651: uuidv7() };
const stressRun = links.modbusServer.slice(-8);
const serverPorts = { modbus: 15301, s7: 15302, sl651: 15303 };
const devices = {
    modbusServer: [] as string[],
    modbusClient: [] as string[],
    s7Server: [] as string[],
    s7Client: [] as string[],
    sl651Server: [] as string[],
};

const modbusTargets = Array.from({ length: deviceCount }, (_, index) => ({
    id: uuidv7(),
    name: `Modbus Target ${index}`,
    ip: '127.0.0.1',
    port: 16000 + index,
    status: 'enabled',
}));
const s7Targets = Array.from({ length: deviceCount }, (_, index) => ({
    id: uuidv7(),
    name: `S7 Target ${index}`,
    ip: '127.0.0.1',
    port: 16100 + index,
    status: 'enabled',
}));

const statements: string[] = [
    'BEGIN',
    "DELETE FROM device_data WHERE device_id IN (SELECT id FROM device WHERE name LIKE 'Stress %')",
    "DELETE FROM device WHERE name LIKE 'Stress %'",
    "DELETE FROM protocol_config p WHERE p.name LIKE 'Stress %' AND NOT EXISTS (SELECT 1 FROM device d WHERE d.protocol_config_id = p.id)",
    "DELETE FROM link l WHERE l.name LIKE 'Stress %' AND NOT EXISTS (SELECT 1 FROM device d WHERE d.link_id = l.id)",
];

function addLink(
    id: string,
    name: string,
    mode: 'TCP Server' | 'TCP Client',
    protocol: 'Modbus' | 'S7' | 'SL651',
    ip: string,
    port: number,
    targets: unknown[],
) {
    const endpoint = {
        mode,
        ip: mode === 'TCP Server' ? ip : '',
        port: mode === 'TCP Server' ? port : 0,
        targets: mode === 'TCP Client' ? targets : [],
    };
    statements.push(
        `INSERT INTO link(id,name,protocol,endpoint,status,created_by) VALUES (${quote(id)},${quote(name)},${quote(protocol)},${json(endpoint)},'enabled',${quote(adminId)})`,
    );
}

addLink(links.modbusServer, `Stress Modbus Server ${stressRun}`, 'TCP Server', 'Modbus', '0.0.0.0', serverPorts.modbus, []);
addLink(links.modbusClient, `Stress Modbus Client ${stressRun}`, 'TCP Client', 'Modbus', '', 0, modbusTargets);
addLink(links.s7Server, `Stress S7 Server ${stressRun}`, 'TCP Server', 'S7', '0.0.0.0', serverPorts.s7, []);
addLink(links.s7Client, `Stress S7 Client ${stressRun}`, 'TCP Client', 'S7', '', 0, s7Targets);
addLink(links.sl651Server, `Stress SL651 Server ${stressRun}`, 'TCP Server', 'SL651', '0.0.0.0', serverPorts.sl651, []);

const modbusRegisters: Array<Record<string, unknown>> = [
    {
        id: uuidv7(),
        name: 'Coil BOOL',
        dataType: 'BOOL',
        registerType: 'COIL',
        address: 0,
        quantity: 1,
        writable: true,
    },
    {
        id: uuidv7(),
        name: 'Discrete BOOL',
        dataType: 'BOOL',
        registerType: 'DISCRETE_INPUT',
        address: 0,
        quantity: 1,
    },
];
const modbusWordTypes = [
    ['INT16', 1],
    ['UINT16', 1],
    ['INT32', 2],
    ['UINT32', 2],
    ['FLOAT32', 2],
    ['INT64', 4],
    ['UINT64', 4],
    ['DOUBLE', 4],
] as const;
const modbusByteOrders = [
    'BIG_ENDIAN',
    'LITTLE_ENDIAN',
    'BIG_ENDIAN_BYTE_SWAP',
    'LITTLE_ENDIAN_BYTE_SWAP',
] as const;
for (const registerType of ['HOLDING_REGISTER', 'INPUT_REGISTER'] as const) {
    let address = 0;
    for (const byteOrder of modbusByteOrders) {
        for (const [dataType, quantity] of modbusWordTypes) {
            modbusRegisters.push({
                id: uuidv7(),
                name: `${registerType} ${dataType} ${byteOrder}`,
                unit: 'u',
                dataType,
                byteOrder,
                registerType,
                address,
                quantity,
                scale: 1,
                decimals: dataType === 'FLOAT32' || dataType === 'DOUBLE' ? 2 : -1,
                writable: registerType === 'HOLDING_REGISTER',
            });
            address += quantity;
        }
    }
}

const s7Areas: Array<Record<string, unknown>> = [];
const s7DataTypes = [
    ['BOOL', 1],
    ['INT8', 1],
    ['UINT8', 1],
    ['INT16', 2],
    ['UINT16', 2],
    ['INT32', 4],
    ['UINT32', 4],
    ['FLOAT', 4],
    ['LREAL', 8],
    ['STRING', 8],
] as const;
for (const area of ['DB', 'V', 'MK'] as const) {
    let start = 0;
    for (const [dataType, size] of s7DataTypes) {
        s7Areas.push({
            id: uuidv7(),
            name: `${area} ${dataType}`,
            dataType,
            area,
            dbNumber: area === 'DB' ? 1 : undefined,
            start,
            startBit: dataType === 'BOOL' ? 3 : 0,
            size,
            decimals: dataType === 'FLOAT' || dataType === 'LREAL' ? 2 : -1,
            writable: area === 'DB' || area === 'V' || area === 'MK',
        });
        start += size;
    }
}
for (const area of ['PE', 'PA'] as const)
    s7Areas.push({
        id: uuidv7(),
        name: `${area} BOOL`,
        dataType: 'BOOL',
        area,
        start: 0,
        startBit: 3,
        size: 1,
        writable: area === 'PA',
    });
for (const area of ['CT', 'TM'] as const)
    s7Areas.push({
        id: uuidv7(),
        name: `${area} UINT16`,
        dataType: 'UINT16',
        area,
        start: 0,
        size: 2,
    });

const sl651Functions = Array.from({ length: 0x20 }, (_, index) => {
    const funcCode = (0x30 + index).toString(16).toUpperCase().padStart(2, '0');
    const encode = ['BCD', 'TIME_YYMMDDHHMMSS', 'JPEG', 'DICT', 'HEX'][index % 5];
    const length = encode === 'TIME_YYMMDDHHMMSS' ? 6 : encode === 'BCD' ? 2 : 3;
    const guideHex = `39${index.toString(16).toUpperCase().padStart(2, '0')}`;
    const direction = index % 2 === 0 ? 'UP' : 'DOWN';
    const element = {
        id: uuidv7(),
        name: `SL651 ${funcCode} ${encode}`,
        unit: '',
        guideHex,
        encode,
        length,
        digits: encode === 'BCD' ? 2 : 0,
    };
    return {
        funcCode,
        dir: direction,
        name: `Stress ${funcCode}`,
        elements: [element],
        ...(direction === 'DOWN'
            ? { responseElements: [{ ...element, id: uuidv7(), name: `${element.name} response` }] }
            : {}),
    };
});
statements.push(
    `INSERT INTO protocol_config(id,protocol,name,enabled,config,created_by) VALUES (${quote(protocols.Modbus)},'Modbus',${quote(`Stress Modbus ${stressRun}`)},TRUE,${json({ readInterval: 2, byteOrder: 'BIG_ENDIAN', packet: { mergeGap: 0, maxQuantity: 125 }, registers: modbusRegisters })},${quote(adminId)})`,
    `INSERT INTO protocol_config(id,protocol,name,enabled,config,created_by) VALUES (${quote(protocols.S7)},'S7',${quote(`Stress S7 ${stressRun}`)},TRUE,${json({ pollInterval: 2, plcModel: 'S7-1200', connection: { mode: 'RACK_SLOT', connectionType: 'PG', rack: 0, slot: 1 }, areas: s7Areas })},${quote(adminId)})`,
    `INSERT INTO protocol_config(id,protocol,name,enabled,config,created_by) VALUES (${quote(protocols.SL651)},'SL651',${quote(`Stress SL651 ${stressRun}`)},TRUE,${json({ responseMode: 'M1', funcs: sl651Functions })},${quote(adminId)})`,
);

function addDevice(
    bucket: keyof typeof devices,
    index: number,
    linkId: string,
    protocolId: string,
    code: string,
    targetId: string | null,
    registration: string | null,
    heartbeat: string | null,
    modbusMode: string | null,
) {
    const id = uuidv7();
    devices[bucket].push(id);
    const protocolParams = {
        device_code: code,
        ...(targetId ? { target_id: targetId } : {}),
        online_timeout: 5,
        remote_control: true,
        ...(modbusMode ? { modbus_mode: modbusMode } : {}),
        slave_id: index + 1,
        timezone: '+00:00',
        heartbeat: heartbeat ? { mode: 'ASCII', content: heartbeat } : { mode: 'OFF' },
        registration: registration ? { mode: 'ASCII', content: registration } : { mode: 'OFF' },
    };
    statements.push(`INSERT INTO device(
id,name,link_id,protocol_config_id,status,protocol_params,created_by) VALUES (
${quote(id)},${quote(`Stress ${bucket} ${index}`)},${quote(linkId)},
${quote(protocolId)},'enabled',${json(protocolParams)},${quote(adminId)})`);
}

for (let index = 0; index < deviceCount; index++) {
    addDevice('modbusServer', index, links.modbusServer, protocols.Modbus, `MS${pad(index, 3)}`, null, `M${pad(index, 3)}`, `HM${pad(index, 3)}`, index % 2 ? 'RTU' : 'TCP');
    addDevice('modbusClient', index, links.modbusClient, protocols.Modbus, `MC${pad(index, 3)}`, modbusTargets[index].id, null, null, index % 2 ? 'RTU' : 'TCP');
    addDevice('s7Server', index, links.s7Server, protocols.S7, `SS${pad(index, 3)}`, null, `S${pad(index, 3)}`, `HS${pad(index, 3)}`, null);
    addDevice('s7Client', index, links.s7Client, protocols.S7, `SC${pad(index, 3)}`, s7Targets[index].id, null, null, null);
    addDevice('sl651Server', index, links.sl651Server, protocols.SL651, pad(index + 1, 10), null, null, null, null);
}
statements.push('COMMIT');

const databaseProcess = Bun.spawn(
    [
        'docker',
        'exec',
        '-i',
        '-e',
        `PGPASSWORD=${Bun.env.DB_PASSWORD ?? ''}`,
        'timescaledb',
        'psql',
        '-v',
        'ON_ERROR_STOP=1',
        '-U',
        Bun.env.DB_USERNAME ?? 'postgres',
        '-d',
        Bun.env.DB_DATABASE ?? 'postgres',
    ],
    { stdin: new Blob([`${statements.join(';\n')};\n`]), stdout: 'inherit', stderr: 'inherit' },
);
if ((await databaseProcess.exited) !== 0) throw new Error('stress fixture database setup failed');

await Bun.write(
    'build/stress-fixture.json',
    JSON.stringify(
        {
            deviceCount,
            links,
            protocols,
            targets: { modbus: modbusTargets, s7: s7Targets },
            serverPorts,
            devices,
            coverage: {
                modbusElementIds: modbusRegisters.map((item) => item.id),
                s7ElementIds: s7Areas.map((item) => item.id),
                sl651ElementIds: sl651Functions.flatMap((item) => [
                    ...item.elements.map((element) => element.id),
                    ...(item.responseElements?.map((element) => element.id) ?? []),
                ]),
                sl651TelemetryElementIds: sl651Functions.flatMap((item) =>
                    item.dir === 'DOWN'
                        ? (item.responseElements?.map((element) => element.id) ?? [])
                        : item.elements.map((element) => element.id),
                ),
                sl651FunctionCodes: sl651Functions.map((item) => item.funcCode),
                latestSamples: [
                    ...modbusRegisters.map((item) => ({ deviceCode: 'MC000', elementId: item.id })),
                    ...s7Areas.map((item) => ({ deviceCode: 'SC000', elementId: item.id })),
                    ...sl651Functions.map((item, index) => ({
                        deviceCode: pad(index + 1, 10),
                        elementId:
                            item.dir === 'DOWN'
                                ? item.responseElements?.[0]?.id
                                : item.elements[0]?.id,
                    })),
                ],
            },
        },
        null,
        2,
    ),
);
console.log(
    `stress fixture created: links=5 devices=${deviceCount * 5} ` +
        `modbus_elements=${modbusRegisters.length} s7_elements=${s7Areas.length} ` +
        `sl651_functions=${sl651Functions.length}`,
);
