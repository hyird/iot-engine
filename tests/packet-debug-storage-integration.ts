import assert from 'node:assert/strict';
import {redisUrl} from './architecture-fixture';
const redis = new Bun.RedisClient(redisUrl);
const source = await Bun.file('service/features/packet_log/packet_log.service.h').text();
const start = source.indexOf('local fields={}');
const script = source.slice(start,source.indexOf(')lua";',start));
const tag = crypto.randomUUID();
const identity=(name:string)=>`${tag}:${name}`;
const key=(type:string,name:string)=>`iot:debug:v3:${type}:${identity(name)}`;
const cleanup=new Set<string>([key('link','scope'),key('device','scope')]);
async function write(round:string,packet:string,changes:Record<string,string>={}) {
    for(const value of [key('packet',packet),key('acquisition',round),key('acquisition',round)+':packets',key('acquisition',round)+':retired'])cleanup.add(value);
    const fields={acquisition_prefix:'iot:debug:v3:acquisition:',acquisition_id:identity(round),event_id:identity(packet),link_id:identity('scope'),device_id:identity('scope'),direction:'RX',source:'edge',payload_hex:'AABB',offset:'0',time_ms:'1000',transport_status:'received',parse_status:'pending',storage_status:'pending',acquisition_state:'running',...changes};
    return redis.send('EVAL',[script,'2',key('link','scope'),key('device','scope'),...Object.entries(fields).flat()]);
}
try {
    await write('round','one');await write('round','two');
    const revision=await redis.send('HGET',[key('packet','one'),'revision']);
    const created=await redis.send('HGET',[key('acquisition','round'),'created_ms']);
    await write('round','one');
    assert.equal(await redis.send('HGET',[key('packet','one'),'revision']),revision);
    assert.equal(Number(await redis.send('ZCARD',[key('acquisition','round')+':packets'])),2);
    assert.equal(Number(await redis.send('ZCARD',[key('device','scope')])),1);
    await write('round','one',{storage_status:'stored',history_id:identity('round'),parsed_json:'{"values":{"level":1}}',acquisition_state:'success'});
    await write('round','one',{parsed_json:'{"values":{}}'});
    assert.equal(await redis.send('HGET',[key('acquisition','round'),'parsed_json']),'{"values":{"level":1}}');
    assert.equal(await redis.send('HGET',[key('acquisition','round'),'state']),'success');
    assert.equal(await redis.send('HGET',[key('acquisition','round'),'created_ms']),created);
    await assert.rejects(write('wrong','one'));
    await assert.rejects(write('round','one',{payload_hex:'FFFF'}));
    await write('large','fragmented',{offset:'4096',payload_hex:'CCDD'});
    await write('large','fragmented',{payload_hex:'AA'.repeat(4096)});
    assert.equal(await redis.send('HGET',[key('packet','fragmented'),'payload_hex']),'AA'.repeat(4096)+'CCDD');
    assert.equal(Number(await redis.send('ZCARD',[key('acquisition','large')+':packets'])),1);
    await write('empty','metadata',{payload_hex:''});
    assert.equal(Number(await redis.send('EXISTS',[key('acquisition','empty')])),1);
    assert.equal(Number(await redis.send('EXISTS',[key('packet','metadata')])),0);
    for(let index=0;index<101;index++)await write(`retained-${index}`,`packet-${index}`);
    assert.equal(Number(await redis.send('ZCARD',[key('device','scope')])),100);
    assert.equal(Number(await redis.send('EXISTS',[key('packet','one')])),0);
    await write('round','one');
    assert.equal(Number(await redis.send('EXISTS',[key('packet','one')])),0,'late event resurrected retired round');
    console.log('PASS round storage: identities, replay, state, fragments, empty rounds, whole-round retention');
} finally {
    await redis.send('DEL',[...cleanup]);
    redis.close();
}
