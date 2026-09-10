// Uses only the disposable architecture Redis fixture.
import assert from 'node:assert/strict';
const redis = new Bun.RedisClient('redis://127.0.0.1:56439');
const source = await Bun.file('service/features/edge/session/session.service.h').text();
const script = (name: string) => {
    const position = source.indexOf(`${name} =`);
    assert(position >= 0);
    return source.slice(position).match(/R"lua\(([\s\S]*?)\)lua"/)![1];
};
const prefix = `test:edge-session:${crypto.randomUUID()}`;
const keys = [`${prefix}:node`, `${prefix}:deadlines`, `${prefix}:changes`];
const run = (name: string, args = ['old-session'], selected = keys) =>
    redis.send('EVAL', [script(name), String(selected.length), ...selected, ...args]);
try {
    assert.equal(await run('kClaimScript'), 1);
    assert.equal(await redis.send('XLEN', [keys[2]]), 1);
    assert.equal(await run('kRefreshScript'), 1);
    assert.equal(await redis.send('XLEN', [keys[2]]), 1, 'renewal must not trigger query churn');
    assert.equal(await run('kClaimScript', ['new-session']), 1);
    assert.equal(await run('kRefreshScript'), 0, 'old process cannot renew new session');
    assert.equal(await run('kReleaseScript'), 0, 'old close cannot remove new session');
    assert.equal(await redis.send('GET', [keys[0]]), 'new-session');
    assert.equal(await run('kReleaseScript'), 0, 'old close cannot remove new session');
    assert.equal(await run('kReleaseScript', ['new-session']), 1);
    assert.equal(await redis.send('EXISTS', [keys[0]]), 0);
    assert.equal(await redis.send('ZCARD', [keys[1]]), 0);
    assert.equal(await redis.send('XLEN', [keys[2]]), 3);
    await run('kClaimScript');
    await redis.send('PEXPIRE', [keys[0], '1']);
    await redis.send('ZADD', [keys[1], '0', keys[0]]);
    await Bun.sleep(10);
    assert.equal(await run('kExpireScript', [], [keys[1], keys[2]]), 1);
    assert.equal(await run('kExpireScript', [], [keys[1], keys[2]]), 0);
    assert.equal(await redis.send('XLEN', [keys[2]]), 5, 'crashed owner emits one offline event');
    console.log('PASS session ownership, renewal, disconnect and expiry query events');
} finally {
    await redis.send('DEL', keys);
    redis.close();
}
