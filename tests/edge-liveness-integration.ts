// Real WebSocket probes against the disposable fixture; no firmware is changed.
import assert from 'node:assert/strict';
const db=new Bun.SQL('postgres://architecture_test@127.0.0.1:55439/iot_architecture');
const redis=new Bun.RedisClient('redis://127.0.0.1:56439');
const platform='00000000-0000-7000-8000-000000000001';
const bytes=(id:string)=>Buffer.from(id.replaceAll('-',''),'hex');
function integer(n:bigint|number) {
    let value=BigInt(n);const out:number[]=[];
    do {let byte=Number(value&127n);value>>=7n;if(value)byte|=128;out.push(byte);} while(value);
    return Buffer.from(out);
}
const field=(tag:number,value:bigint|number|Buffer|string)=>{
    if(typeof value==='bigint'||typeof value==='number')return Buffer.concat([integer(tag*8),integer(value)]);
    const data=typeof value==='string'?Buffer.from(value):value;
    return Buffer.concat([integer(tag*8+2),integer(data.length),data]);
};
function decode(data:Buffer) {
    const result=new Map<number,bigint|Buffer>();let position=0;
    const read=()=>{let n=0n,shift=0n;for(;;){const b=data[position++];assert(b!==undefined);n|=BigInt(b&127)<<shift;if(!(b&128))return n;shift+=7n;}};
    while(position<data.length) {const tag=Number(read()),wire=tag&7;
        if(wire===0)result.set(tag>>3,read());
        else if(wire===2){const size=Number(read());result.set(tag>>3,data.subarray(position,position+size));position+=size;}
        else throw Error(`Unexpected probe wire type ${wire}`);
    }return result;
}
function imei() {
    const base='99'+String(Math.floor(Math.random()*1e12)).padStart(12,'0');
    let sum=0;for(let i=0;i<14;i++){let digit=Number(base[i])*(i%2?2:1);if(digit>9)digit-=9;sum+=digit;}
    return base+String((10-sum%10)%10);
}
const probes:Array<{node:string;imei:string;socket:WebSocket;pings:number;closed:boolean;version:number;respond:boolean}>=[];
try {
    for(const [version,respond] of [[2,true],[5,true],[6,true],[2,false]] as const) {
        const node=crypto.randomUUID(),identity=imei();
        await db`INSERT INTO edge_node(id,platform_id,imei,enrollment_status) VALUES(${node},${platform},${identity},'approved')`;
        await redis.send('SET',[`iot:edge:auth:${identity}`,`${node}|approved`]);
        const socket=new WebSocket('ws://127.0.0.1:55102/edge/v1/connect');socket.binaryType='arraybuffer';
        const probe={node,imei:identity,socket,pings:0,closed:false,version,respond};probes.push(probe);
        let sequence=1n,epoch=0n;
        const envelope=(tag:number,payload:Buffer)=>Buffer.concat([
            field(1,version),field(2,bytes(crypto.randomUUID())),field(3,bytes(node)),field(4,bytes(platform)),
            field(5,epoch),field(6,Date.now()),field(8,sequence++),field(tag,payload)]);
        await new Promise<void>((resolve,reject)=>{
            const timeout=setTimeout(()=>reject(Error('Legacy Hello timed out')),10000);
            socket.onopen=()=>socket.send(envelope(20,Buffer.concat([field(1,identity),field(2,'fixture'),field(3,'0.3.38')])));
            socket.onerror=()=>{clearTimeout(timeout);reject(Error('Fixture WebSocket failed'));};
            socket.onclose=()=>{probe.closed=true;clearTimeout(timeout);reject(Error('Fixture closed before HelloAck'));};
            socket.onmessage=(event)=>{
                const message=decode(Buffer.from(event.data as ArrayBuffer));
                assert.equal(Number(message.get(1)),version,'negotiated legacy version changed');
                if(message.has(21)){epoch=message.get(5) as bigint;clearTimeout(timeout);resolve();}
                if(message.has(80)) {
                    probe.pings++;
                    if(respond)socket.send(envelope(81,message.get(80) as Buffer));
                }
            };
        });
    }
    await Bun.sleep(65000);
    for(const probe of probes) {
        assert(probe.pings>=2,`protocol ${probe.version}: no application probes`);
        assert.equal(probe.closed,!probe.respond,`protocol ${probe.version}: incorrect response-based liveness`);
        const ttl=Number(await redis.send('TTL',[`iot:edge:session:${probe.node}`]));
        if(probe.respond)assert(ttl>50,`protocol ${probe.version}: responsive legacy lease expired`);
        else assert.equal(ttl,-2,'silent node lease was kept alive by the server');
    }
    console.log('PASS protocol 2/5/6 Ping/Pong retain responsive sessions; silent node closes without lease renewal');
} finally {
    for(const probe of probes)probe.socket.close();
    await Bun.sleep(100);
    for(const probe of probes)await redis.send('DEL',[`iot:edge:auth:${probe.imei}`]);
    await db.close();redis.close();
}
