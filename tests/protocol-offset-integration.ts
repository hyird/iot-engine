import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { apiBase, databaseUrl } from './architecture-fixture';

const sql = new Bun.SQL(databaseUrl);
const tag = `offset_${crypto.randomUUID().replaceAll('-', '').slice(0, 12)}`;
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({
    iss: 'iot-engine', aud: 'iot-engine-web', sub: '00000000-0000-7000-8000-000000000002',
    user_id: '00000000-0000-7000-8000-000000000002', username: 'admin',
    token_type: 'access', iat: now, exp: now + 3600,
})}`;
const token = `${unsigned}.${createHmac('sha256', 'architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;

// Raw JSON preserves exponent notation and rejects numeric strings at the API boundary.
function payload(name: string, offset: string | undefined, length: string, field: string) {
    const element = `{"id":"offset-element","name":"偏移量","positionMode":"OFFSET","encode":"BCD","digits":0,"length":${length}${offset === undefined ? '' : `,"byteOffset":${offset}`}}`;
    return `{"name":${JSON.stringify(name)},"protocol":"SL651","config":{"responseMode":"M3","storagePolicy":"report","funcs":[{"id":"offset-function","funcCode":"E1","dir":"UP","name":"偏移测试","${field}":[${element}]}]}}`;
}

async function write(method: string, path: string, body: string, valid: boolean) {
    const response = await fetch(apiBase + path, {
        method, headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
        body, signal: AbortSignal.timeout(10000),
    });
    const text = await response.text();
    assert.equal(response.status, valid ? 200 : 400, `${method} ${body}: ${text}`);
    if (!valid) assert.equal(JSON.parse(text).code, 16004, text);
}

const cases: [string | undefined, string, boolean][] = [
    [undefined, '1', true], ['0', '8388608', true], ['8388607', '1', true],
    ['1e3', '2e0', true], ['8388607', '2', false], ['8388608', '1', false],
    ['-1', '1', false], ['0.5', '1', false], ['"1"', '1', false],
    ['null', '1', false], ['true', '1', false], ['1e100', '1', false],
    ['0', '0', false], ['0', '8388609', false], ['0', '"1"', false],
    ['0', '1.5', false], ['0', 'null', false],
];
try {
    const baseName = `${tag}_base`;
    await write('POST', '/v1/protocol/configs', payload(baseName, '0', '1', 'elements'), true);
    const [base] = await sql`SELECT id FROM protocol_config WHERE name=${baseName}`;
    assert(base);
    let index = 0;
    for (const field of ['elements', 'responseElements']) {
        for (const [offset, length, valid] of cases) {
            const name = `${tag}_${index++}`;
            const body = payload(name, offset, length, field);
            await write('POST', '/v1/protocol/configs', body, valid);
            const [before] = await sql`SELECT config FROM protocol_config WHERE id=${base.id}`;
            await write('PUT', `/v1/protocol/configs/${base.id}`, payload(baseName, offset, length, field), valid);
            const [after] = await sql`SELECT config FROM protocol_config WHERE id=${base.id}`;
            if (!valid) assert.deepEqual(after.config, before.config, 'rejected update changed stored config');
            else assert.deepEqual(after.config, JSON.parse(body).config, 'accepted config changed during storage');
        }
    }
    console.log('PASS SL651 OFFSET create/update: defaults, exact boundary, exponent normalization, malformed values and failed-write isolation');
} finally {
    await sql`DELETE FROM protocol_config WHERE name LIKE ${tag + '%'}`;
    await sql.close();
}
