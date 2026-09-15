import assert from 'node:assert/strict';

const redisUrl = Bun.env.ARCHITECTURE_REDIS_URL;
assert(redisUrl, 'ARCHITECTURE_REDIS_URL must point to a disposable Redis fixture');
const redis = new Bun.RedisClient(redisUrl);
const key = `iot:test:gb-owner:${crypto.randomUUID()}`;
const source = await Bun.file('service/features/gb28181/gb28181.service.h').text();
function script(method: string) {
    const start = source.indexOf(`${method}(`);
    assert(start >= 0, `missing production method: ${method}`);
    const lua = source.slice(start).match(/R"lua\(([\s\S]*?)\)lua"/)?.[1];
    assert(lua, `missing production Lua: ${method}`);
    return lua;
}
const claim = script('claimOwnerLease');
const renew = script('renewOwnerLease');
const release = script('releaseOwnerLease');
const evaluate = (lua: string, ...args: string[]) => redis.send('EVAL', [lua, '1', key, ...args]);

try {
    assert.equal(Number(await evaluate(renew, 'first', '15000')), 0);
    assert.equal(Number(await redis.send('EXISTS', [key])), 0);
    assert.equal(Number(await evaluate(claim, 'first', '', '15000')), 1);
    assert.equal(Number(await evaluate(claim, 'intruder', '', '15000')), 0);
    assert.equal(Number(await evaluate(claim, 'next', 'wrong', '15000')), 0);
    assert.equal(Number(await evaluate(claim, 'next', 'first', '15000')), 1);
    assert.equal(Number(await evaluate(renew, 'first', '15000')), 0);
    assert.equal(Number(await evaluate(release, 'first')), 0);
    assert.equal(await redis.send('GET', [key]), 'next');
    assert.equal(Number(await evaluate(renew, 'next', '15000')), 1);
    const ttl = Number(await redis.send('PTTL', [key]));
    assert(ttl > 0 && ttl <= 15000);
    assert.equal(Number(await evaluate(release, 'next')), 1);
    assert.equal(Number(await evaluate(renew, 'next', '15000')), 0);
    assert.equal(Number(await redis.send('EXISTS', [key])), 0);
    console.log('PASS GB28181 owner replacement, renewal and release fencing');
} finally {
    await redis.send('DEL', [key]);
    redis.close();
}
