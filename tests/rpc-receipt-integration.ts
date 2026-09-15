import assert from 'node:assert/strict';
import { redisUrl } from './architecture-fixture';

const redis = new Bun.RedisClient(redisUrl);
const prefix = `iot:test:rpc-receipt:${crypto.randomUUID()}`;
const input = `${prefix}:input`;
const reply = `${prefix}:reply`;
const changes = `${prefix}:changes`;
const source = await Bun.file('service/features/messaging/messaging.service.h').text();
const start = source.indexOf('kReplyScript =');
assert(start >= 0);
const lua = source.slice(start).match(/R"lua\(([\s\S]*?)\)lua"/)?.[1];
assert(lua);
try {
    await redis.send('XGROUP', ['CREATE', input, 'group', '0', 'MKSTREAM']);
    const id = String(await redis.send('XADD', [input, '*', 'id', 'request']));
    await redis.send('XREADGROUP', ['GROUP', 'group', 'worker', 'STREAMS', input, '>']);
    const payload = '{"code":0,"data":{"value":"保持原始 JSON"}}';
    assert.equal(Number(await redis.send('EVAL', [lua, '3', reply, changes, input,
        payload, '120', 'group', id])), 1);
    assert.equal(await redis.send('GET', [reply]), payload);
    const ttl = Number(await redis.send('TTL', [reply]));
    assert(ttl > 0 && ttl <= 120);
    assert.equal(Number(await redis.send('XLEN', [input])), 0);
    const pending = await redis.send('XPENDING', [input, 'group']) as unknown[];
    assert.equal(Number(pending[0]), 0);
    const notifications = await redis.send('XRANGE', [changes, '-', '+']) as unknown as [string, string[]][];
    assert.deepEqual(notifications[0][1], ['topic', reply, 'schema_version', '1']);
    console.log('PASS RPC reply persistence, expiry, notification and input acknowledgement');
} finally {
    await redis.send('DEL', [input, reply, changes]);
    redis.close();
}
