import assert from 'node:assert/strict';

const redisUrl = Bun.env.ARCHITECTURE_REDIS_URL;
assert(redisUrl, 'ARCHITECTURE_REDIS_URL must point to a disposable Redis fixture');
const redis = new Bun.RedisClient(redisUrl);
const prefix = `iot:test:gb-projection:${crypto.randomUUID()}`;
const stream = `${prefix}:stream`;
const marker = `${prefix}:sent`;
const source = await Bun.file('service/features/gb28181/gb28181.service.h').text();
const start = source.indexOf('publishProjection(');
assert(start >= 0);
const lua = source.slice(start).match(/R"lua\(([\s\S]*?)\)lua"/)?.[1];
assert(lua, 'missing production projection Lua');
const publish = (order: string, current: string) => redis.send('EVAL', [
    lua, '2', stream, marker, order, current, 'projection_id', prefix,
]) as Promise<string[]>;

try {
    const first = await publish('', '');
    assert.equal(first.length, 2);
    assert.equal(first[0], first[1]);
    assert.deepEqual(await publish('', ''), first);
    assert.equal(Number(await redis.send('XLEN', [stream])), 1);
    await redis.send('DEL', [marker]);
    assert.deepEqual(await publish(first[0], first[1]), first);
    assert.equal(Number(await redis.send('XLEN', [stream])), 1);
    await redis.send('XDEL', [stream, first[1]]);
    const repaired = await publish(first[0], first[1]);
    assert.equal(repaired[0], first[0]);
    assert.notEqual(repaired[1], first[1]);
    assert.deepEqual(await publish(first[0], first[1]), repaired);
    assert.equal(Number(await redis.send('XLEN', [stream])), 1);
    const entries = await redis.send('XRANGE', [stream, repaired[1], repaired[1]]);
    assert(JSON.stringify(entries).includes('projection_order'));
    await assert.rejects(publish('invalid', repaired[1]));
    assert.equal(Number(await redis.send('XLEN', [stream])), 1);
    console.log('PASS GB28181 projection deduplication, marker loss and ordered repair');
} finally {
    await redis.send('DEL', [stream, marker]);
    redis.close();
}
