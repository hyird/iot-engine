import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { databaseUrl,redisUrl,apiBase } from './architecture-fixture';
const db=new Bun.SQL(databaseUrl),redis=new Bun.RedisClient(redisUrl);
const admin='00000000-0000-7000-8000-000000000002';
const uuid=()=>crypto.randomUUID();
const encode=(value:unknown)=>Buffer.from(JSON.stringify(value)).toString('base64url');
const now=Math.floor(Date.now()/1000);
const unsigned=`${encode({alg:'HS256',typ:'JWT'})}.${encode({iss:'iot-engine',aud:'iot-engine-web',sub:admin,user_id:admin,username:'debug-test',token_type:'access',iat:now,exp:now+3600})}`;
const token=`${unsigned}.${createHmac('sha256','architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;
async function toggle(scope:string,id:string,enabled:boolean) {
    const response=await fetch(`${apiBase}/v1/${scope}/${id}/debug`,{method:'PUT',headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json'},body:JSON.stringify({enabled})});
    const body=await response.text();assert.equal(response.status,200,body);
}
async function until(condition:()=>Promise<boolean>,reason:string) {
    const deadline=Date.now()+20000;while(Date.now()<deadline){if(await condition())return;await Bun.sleep(50);}throw Error(reason);
}
let connections=0,requests=0;
const server=Bun.listen<{buffer:Buffer}>({hostname:'127.0.0.1',port:0,socket:{
    open(socket){++connections;socket.data={buffer:Buffer.alloc(0)};},
    data(socket,input){
        socket.data.buffer=Buffer.concat([socket.data.buffer,Buffer.from(input)]);
        while(socket.data.buffer.length>=12){
            const request=socket.data.buffer.subarray(0,12);socket.data.buffer=socket.data.buffer.subarray(12);
            assert.equal(request[7],3);++requests;
            const reply=Buffer.from([request[0],request[1],0,0,0,5,request[6],3,2,0,request[6]*10]);socket.write(reply);
        }
    },error(_socket,error){throw error;},close(){}
}});
const link=uuid(),target=uuid(),model=uuid(),device1=uuid(),device2=uuid(),point=uuid();
const length=(scope:string,id:string)=>redis.send('XLEN',[`iot:debug:packets:${scope}:${id}`]).then(Number);
try {
    const config={storagePolicy:'report',readInterval:1,byteOrder:'BIG_ENDIAN',registers:[{id:point,name:'value',registerType:'HOLDING_REGISTER',dataType:'UINT16',address:0,quantity:1,scale:1}]};
    await db`INSERT INTO protocol_config(id,name,protocol,config,created_by) VALUES(${model},${model},'Modbus',${config}::jsonb,${admin})`;
    const endpoint={transport:'tcp',mode:'TCP Client',ip:'',port:0,targets:[{id:target,name:'local',ip:'127.0.0.1',port:server.port,status:'enabled'}]};
    await db`INSERT INTO link(id,name,protocol,endpoint,status,execution,created_by) VALUES(${link},${link},'Modbus',${endpoint}::jsonb,'enabled','collector',${admin})`;
    for(const [device,slave] of [[device1,1],[device2,2]] as const){
        const params={device_code:String(slave),target_id:target,modbus_mode:'TCP',slave_id:slave};
        await db`INSERT INTO device(id,name,link_id,protocol_config_id,protocol_params,created_by) VALUES(${device},${device},${link},${model},${params}::jsonb,${admin})`;
    }
    await toggle('device',device1,true);
    await until(async()=>await length('device',device1)>=2,'device debug did not capture direct TX/RX');
    assert.equal(await length('device',device2),0,'device debug leaked sibling traffic');
    const rows=await redis.send('XRANGE',[`iot:debug:packets:device:${device1}`,'-','+']) as [string,string[]][];
    assert(rows.some(([,fields])=>fields.includes('RX')));
    assert(rows.some(([,fields])=>fields.includes('TX_ATTEMPT')));
    for(const [,fields] of rows){const index=fields.indexOf('payload_hex');assert.equal(fields[index+1].slice(12,14),'01');}
    const opened=connections;
    await toggle('link',link,true);
    await until(async()=>await length('device',device2)>=2,'link debug did not cover sibling');
    await toggle('device',device1,false);
    const before=await length('device',device1);
    await until(async()=>await length('device',device1)>before,'closing device suppressed active link debug');
    assert.equal(connections,opened,'debug toggle reopened TCP connection');
    await toggle('link',link,false);
    const requestBarrier=requests+8;
    await until(async()=>requests>=requestBarrier,'collection stopped after debug was disabled');
    const stopped=await length('link',link), secondBarrier=requests+4;
    await until(async()=>requests>=secondBarrier,'normal sampling stopped');
    assert.equal(await length('link',link),stopped,'packets continued after both switches closed');
    assert((await db`SELECT 1 FROM device_data WHERE device_id=${device1} LIMIT 1`).length,'debug switch broke history persistence');
    const denied=await fetch(`${apiBase}/v1/device/${device1}/debug/packets`,{headers:{Accept:'text/event-stream'}});
    assert.equal(denied.status,401);
    console.log('PASS direct Modbus: TX/RX, sibling isolation, link override, manual close, connection continuity and unchanged history');
} finally {
    server.stop(true);
    await db`UPDATE device SET deleted_at=NOW() WHERE id IN (${device1},${device2})`;
    await db`UPDATE link SET status='disabled',deleted_at=NOW() WHERE id=${link}`;
    await db.close();redis.close();
}
