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
const node=crypto.randomUUID(),identity=imei();let socket:WebSocket|undefined;
try {
    await until(async () => (await fetch(apiBase + '/internal/health/ready')).status === 200,
        'fixture workers are not ready');
    await db`INSERT INTO edge_node(id,platform_id,imei,enrollment_status) VALUES(${node},${platform},${identity},'approved')`;
    await redis.send('SET',[`iot:edge:auth:${identity}`,`${node}|approved`]);
    let sequence=1n,epoch=0n;const received = new Map<number,Buffer[]>();
    const envelope=(tag:number,payload:Buffer)=>Buffer.concat([field(1,6),field(2,bytes(crypto.randomUUID())),field(3,bytes(node)),field(4,bytes(platform)),field(5,epoch),field(6,Date.now()),field(8,sequence++),field(tag,payload)]);
    socket=new WebSocket(apiBase.replace('http:','ws:')+'/edge/v1/connect');socket.binaryType='arraybuffer';
    await new Promise<void>((resolve,reject)=>{
        const timer=setTimeout(()=>reject(Error('Hello timeout')),10000);
        socket!.onopen=()=>socket!.send(envelope(20,Buffer.concat([field(1,identity),field(2,'fixture'),field(3,'0.3.46'),field(23,1)])));
        socket!.onerror=()=>reject(Error('WebSocket error'));
        socket!.onmessage=event=>{
            const message=decode(Buffer.from(event.data as ArrayBuffer));
            if(message.has(21)){epoch=message.get(5) as bigint;clearTimeout(timer);resolve();}
            if(message.has(80))socket!.send(envelope(81,message.get(80) as Buffer));
            for(const tag of [30,31,32,50,41])if(message.has(tag)){
                const queue=received.get(tag)??[];queue.push(message.get(tag) as Buffer);received.set(tag,queue);
            }
        };
    });
    socket.send(envelope(26,field(7,Buffer.from([1,2,3,4,5,6]))));
    await until(async()=> (await db`SELECT 1 FROM edge_node WHERE id=${node} AND capability->'protocols' ? 'FINS'`).length>0,'capabilities not persisted');
    for(const [protocol,number] of [['MC',4],['FINS',5],['DLT645',6]] as const){
        received.clear();const link=crypto.randomUUID(),device=crypto.randomUUID(),point=crypto.randomUUID(),name=crypto.randomUUID();
        const config={storagePolicy:'report',readInterval:1,connection:protocol==='MC'?{frame:'4E',network:2}:protocol==='FINS'?{sourceNode:10,destinationNode:20}:{version:'2007',wakeupBytes:2,writePassword:'00123456',operatorCode:'00000001'},points:[{id:point,name:'edge point',unit:'kWh',writable:true,dataType:protocol==='DLT645'?'BCD':'UINT16',...(protocol==='DLT645'?{identifier:'00000000',length:4,digits:2}:{area:'D',address:100,bit:0,byteOrder:protocol==='MC'?'LITTLE_ENDIAN':'BIG_ENDIAN'})}]};
        await request('/v1/protocol/configs',{name,protocol,config});
        const [model]=await db`SELECT id FROM protocol_config WHERE name=${name}`;
        const endpoint={transport:'tcp',interface:'lo',mode:'TCP Client',ip:'127.0.0.1',port:5001};
        await db`INSERT INTO link(id,name,protocol,endpoint,status,execution,edge_node_id,created_by) VALUES(${link},${name},${protocol},${endpoint}::jsonb,'enabled','edge',${node},${admin})`;
        await db`INSERT INTO device(id,name,link_id,protocol_config_id,protocol_params,created_by) VALUES(${device},${name},${link},${model.id},${{device_code:'000000123456',remote_control:true}}::jsonb,${admin})`;
        await request(`/v1/device/${device}/debug`,{enabled:true},'PUT');
        let deviceConfig:Map<number,bigint|Buffer>|undefined,pointConfig:Map<number,bigint|Buffer>|undefined;
        await until(async()=>{
            for(const payload of received.get(31)??[]){const item=decode(payload);
                if(item.has(11)){const value=decode(item.get(11) as Buffer);if((value.get(1) as Buffer)?.equals(bytes(device)))deviceConfig=value;}
                if(item.has(17)){const value=decode(item.get(17) as Buffer);if((value.get(1) as Buffer)?.equals(bytes(device)))pointConfig=value;}
            }
            return !!deviceConfig && !!pointConfig;
        },`${protocol} configuration not delivered`);
        assert.equal(Number(deviceConfig!.get(5)),number);assert.equal((pointConfig!.get(2) as Buffer).toString(),point);
        const connection=decode(deviceConfig!.get(27) as Buffer);
        if(protocol==='MC'){assert.equal(connection.get(1),1n);assert.equal(connection.get(2),2n);}
        if(protocol==='FINS'){assert.equal(connection.get(8),10n);assert.equal(connection.get(11),20n);}
        if(protocol==='DLT645'){assert.equal(connection.get(13),2007n);assert.equal((connection.get(15) as Buffer).toString('hex'),'00123456');}
        const commits=received.get(32)!;const commit=decode(commits[commits.length-1]);
        socket.send(envelope(33,Buffer.concat([field(1,commit.get(1) as bigint),field(2,commit.get(2) as Buffer)])));
        const report=crypto.randomUUID();
        const scalar=protocol==='DLT645'?Buffer.concat([field(1,7),field(8,'12345678901234.56')]):Buffer.concat([field(1,3),field(4,42)]);
        const record=Buffer.concat([field(1,bytes(report)),field(2,bytes(device)),field(3,bytes(link)),field(4,number),field(5,'POLL'),field(7,'UP'),field(8,Date.now()),field(9,Buffer.concat([field(1,point),field(2,'edge point'),field(3,'kWh'),field(4,scalar)])),field(14,Buffer.from([1,2])),field(15,bytes(report)),field(17,1),field(18,bytes(crypto.randomUUID()))]);
        socket.send(envelope(40,field(1,record)));
        await until(async()=> (await db`SELECT 1 FROM device_data WHERE device_id=${device}`).length>0,`${protocol} edge telemetry not persisted`);
        const [persisted]=await db`SELECT data->'values'->${point}->>'value' AS value FROM device_data WHERE device_id=${device}`;
        assert.equal(persisted.value,protocol==='DLT645'?'12345678901234.56':'42');
        await request(`/v1/device/${device}/commands`,{idempotency_key:crypto.randomUUID(),elements:[{elementId:point,value:protocol==='DLT645'?'13.25':'13'}]});
        await until(async()=> (received.get(50)?.length??0)>0,`${protocol} edge command not delivered`);
        const command=decode(received.get(50)![0]);assert((command.get(2) as Buffer).equals(bytes(device)));
        assert.equal((decode(command.get(3) as Buffer).get(1) as Buffer).toString(),point);
        await db`UPDATE device SET deleted_at=NOW() WHERE id=${device}`;await db`UPDATE link SET deleted_at=NOW(),status='disabled' WHERE id=${link}`;
        console.log(`PASS ${protocol} edge capability, configuration, precise telemetry and command dispatch`);
    }
    socket.send(envelope(26,Buffer.alloc(0)));
    await until(async()=> (await db`SELECT 1 FROM edge_node WHERE id=${node} AND capability->'protocols'='[]'::jsonb`).length>0,'legacy capability did not clear new protocols');
} finally {socket?.close();await redis.send('DEL',[`iot:edge:auth:${identity}`]);redis.close();await db.close();}
