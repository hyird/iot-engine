import assert from 'node:assert/strict';
import { apiBase, redisUrl } from './architecture-fixture';

const redis = new Bun.RedisClient(redisUrl);
const username = `rate_test_${crypto.randomUUID()}`;
const key = `iot:auth:login-failures:${username}`;
async function login() {
    const response = await fetch(`${apiBase}/v1/auth/login`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password: 'invalid-test-password' }),
        signal: AbortSignal.timeout(10000),
    });
    assert(!response.ok);
    return response.json();
}
try {
    for (let failures = 1; failures <= 5; failures++) {
        const response = await login();
        assert.equal(response.code, 11001);
        assert.equal(await redis.send('GET', [key]), String(failures));
    }
    const ttl = Number(await redis.send('PTTL', [key]));
    assert(ttl > 0 && ttl <= 900000);
    await redis.send('PEXPIRE', [key, '10000']);
    const blocked = await login();
    assert.equal(blocked.code, 11003);
    assert.equal(await redis.send('GET', [key]), '5');
    assert(Number(await redis.send('PTTL', [key])) <= 10000);
    await redis.send('DEL', [key]);
    const fresh = await login();
    assert.equal(fresh.code, 11001);
    assert.equal(await redis.send('GET', [key]), '1');
    console.log('PASS login failure storage mapping, lock threshold, fixed expiry and fresh window');
} finally {
    await redis.send('DEL', [key]);
    redis.close();
}
