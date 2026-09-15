import assert from 'node:assert/strict';
import { apiBase, redisUrl } from './architecture-fixture';

const redis = new Bun.RedisClient(redisUrl);
const username = `rate_test_${crypto.randomUUID()}`;
const key = `iot:auth:login-failures:${username}`;
async function login() {
    return fetch(`${apiBase}/v1/auth/login`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password: 'invalid-test-password' }),
        signal: AbortSignal.timeout(10000),
    });
}
try {
    for (let failures = 1; failures <= 5; failures++) {
        const response = await login();
        assert.equal(response.status, failures < 5 ? 401 : 429);
        await response.arrayBuffer();
        assert.equal(await redis.send('GET', [key]), String(failures));
    }
    const ttl = Number(await redis.send('PTTL', [key]));
    assert(ttl > 0 && ttl <= 900000);
    await redis.send('PEXPIRE', [key, '10000']);
    const blocked = await login();
    assert.equal(blocked.status, 429);
    assert.equal((await blocked.json()).code, 11003);
    assert.equal(await redis.send('GET', [key]), '5');
    assert(Number(await redis.send('PTTL', [key])) <= 10000);
    await redis.send('DEL', [key]);
    const fresh = await login();
    assert.equal(fresh.status, 401);
    await fresh.arrayBuffer();
    assert.equal(await redis.send('GET', [key]), '1');
    console.log('PASS login failure storage mapping, lock threshold, fixed expiry and fresh window');
} finally {
    await redis.send('DEL', [key]);
    redis.close();
}
