import assert from 'node:assert/strict';
import { redisUrl } from './architecture-fixture';

const redis = new Bun.RedisClient(redisUrl);
// Execute the actual production transaction against disposable Redis.
const source = await Bun.file('service/features/packet_log/packet_log.service.h').text();
const script = source.match(/static constexpr std::string_view script = R"lua\(([\s\S]*?)\)lua";/)![1];
const tag = crypto.randomUUID();
const link = `iot:debug:v2:link:${tag}`;
const device = `iot:debug:v2:device:${tag}`;
const hash = (id: string) => `iot:debug:v2:packet:${id}`;
const ids: string[] = [];
async function write(id: string, values: Record<string, string>, keys = [link, device]) {
    if (!ids.includes(id)) ids.push(id);
    return redis.send('EVAL', [script, String(keys.length), ...keys,
        ...Object.entries({event_id:id,link_id:tag,device_id:tag,direction:'RX',payload_hex:'0104',time_ms:'1234',...values}).flat()]);
}
try {
    const id = `${tag}:first`;
    await write(id,{transport_status:'received',parse_status:'pending',storage_status:'pending'});
    const revision = await redis.send('HGET',[hash(id),'revision']);
    await write(id,{transport_status:'received',parse_status:'pending',storage_status:'pending'});
    assert.equal(await redis.send('HGET',[hash(id),'revision']), revision, 'replay changed revision');
    await write(id,{parse_status:'success',storage_status:'stored',history_id:'history',parsed_json:'{"values":{}}'});
    await write(id,{parse_status:'pending',storage_status:'pending'});
    await write(id,{parse_status:'success',parsed_json:'{"values":{"uncommitted":1}}'});
    assert.equal(await redis.send('HGET',[hash(id),'parsed_json']),'{"values":{}}',
        'late parse overwrote committed history data');
    await assert.rejects(write(id,{payload_hex:'FFFF'}), /packet identity conflict/);
    assert.equal(await redis.send('HGET',[hash(id),'storage_status']),'stored');
    assert.equal(Number(await redis.send('ZCARD',[link])),1);
    assert.equal(Number(await redis.send('ZCARD',[device])),1);
    assert.equal(await redis.send('TYPE',[hash(id)]),'hash');
    assert(Number(await redis.send('PTTL',[hash(id)]))>0);
    // A real repeated frame with the same bytes is a distinct physical receive.
    await write(`${tag}:repeat`,{transport_status:'received'});
    assert.equal(Number(await redis.send('ZCARD',[device])),2);
    // Trimming one scope must not erase a record still visible in the other.
    for (let index=0;index<501;index++) await write(`${tag}:extra:${index}`,{transport_status:'received'},[link]);
    assert.equal(Number(await redis.send('ZCARD',[link])),500);
    assert.equal(Number(await redis.send('EXISTS',[hash(id)])),1);
    await redis.send('DEL',[device]);
    await redis.send('PEXPIRE',[hash(id),'1']);
    await Bun.sleep(10);
    await write(id,{storage_status:'stored',update_only:'1'});
    assert.equal(Number(await redis.send('EXISTS',[hash(id)])),0,'late completion resurrected expired packet');
    console.log('PASS debug Hash: single body, two indexes, atomic update, replay, late status, physical retransmission, retention and expiration');
} finally {
    await redis.send('DEL',[link,device,...ids.map(hash)]);
    redis.close();
}
