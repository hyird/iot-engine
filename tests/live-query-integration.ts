import { databaseUrl, apiBase } from './architecture-fixture';
// Only the disposable architecture fixture is used. No production defaults.
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { consumeServerSentEvents, type ServerSentEvent } from '../web/utils/sse';

const db = new Bun.SQL(databaseUrl);
const base = apiBase;
const admin = '00000000-0000-7000-8000-000000000002';
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
function token(user: string) {
    const now = Math.floor(Date.now() / 1000);
    const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({
        iss: 'iot-engine', aud: 'iot-engine-web', sub: user, user_id: user,
        username: 'live-test', token_type: 'access', iat: now, exp: now + 3600,
    })}`;
    return `${unsigned}.${createHmac('sha256', 'architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;
}

async function open(path: string, user = admin) {
    const controller = new AbortController();
    const response = await fetch(base + path, {
        headers: { Authorization: `Bearer ${token(user)}`, Accept: 'text/event-stream' },
        signal: controller.signal,
    });
    if (response.status !== 200) throw new Error(`HTTP ${response.status}: ${await response.text()}`);
    assert(response.headers.get('content-type')?.includes('text/event-stream'));
    assert(response.body);
    const events: ServerSentEvent[] = [];
    const waiters: Array<() => void> = [];
    const consumed = consumeServerSentEvents(response.body, (event) => {
        events.push(event);
        for (const wake of waiters.splice(0)) wake();
    }, controller.signal).catch((error) => { if (!controller.signal.aborted) throw error; });
    return {
        async next(kind = 'snapshot') {
            const limit = AbortSignal.timeout(10000);
            for (;;) {
                const index = events.findIndex((event) => event.event === kind);
                if (index >= 0) return JSON.parse(events.splice(index, 1)[0].data);
                await new Promise<void>((resolve, reject) => {
                    const done = () => { limit.removeEventListener('abort', abort); resolve(); };
                    const abort = () => reject(new Error(`Timed out waiting for ${kind}`));
                    if (limit.aborted) { abort(); return; }
                    waiters.push(done);
                    limit.addEventListener('abort', abort, { once: true });
                });
            }
        },
        async close() { controller.abort(); await consumed; },
    };
}

const department = crypto.randomUUID();
const user = crypto.randomUUID();
const role = crypto.randomUUID();
let stream: Awaited<ReturnType<typeof open>> | undefined;
try {
    const rejected = await fetch(`${base}/v1/departments?page=1&pageSize=20`, {
        headers: { Authorization: `Bearer ${token(admin)}`, Accept: 'application/json' },
    });
    assert.equal(rejected.status, 406, 'JSON query alternative must not survive');
    const obsolete = await fetch(`${base}/v1/device/realtime/events`, {
        headers: { Authorization: `Bearer ${token(admin)}` },
    });
    assert.equal(obsolete.status, 404, 'old notification-only endpoint must be removed');
    console.log('PASS old JSON query and notification-only endpoint rejected');

    stream = await open('/v1/departments?page=1&pageSize=20');
    await stream.next();
    await db`INSERT INTO sys_department(id,name,code) VALUES(${department},'SSE external insert',${department})`;
    let snapshot = await stream.next();
    assert(JSON.stringify(snapshot.data).includes('SSE external insert'));
    await db`UPDATE sys_department SET name='SSE external update' WHERE id=${department}`;
    snapshot = await stream.next();
    assert(JSON.stringify(snapshot.data).includes('SSE external update'));
    await stream.close();
    stream = await open('/v1/departments?page=1&pageSize=20');
    snapshot = await stream.next();
    assert(JSON.stringify(snapshot.data).includes('SSE external update'));
    await db`DELETE FROM sys_department WHERE id=${department}`;
    snapshot = await stream.next();
    assert(!JSON.stringify(snapshot.data).includes(department));
    await stream.close();
    stream = undefined;
    console.log('PASS external commit updates SSE; reconnect resets; deletion updates snapshot');

    await db`INSERT INTO sys_user(id,username,password_hash) VALUES(${user},${user},'unused')`;
    await db`INSERT INTO sys_role(id,name,code,permissions) VALUES(${role},${role},${role},'[]'::jsonb)`;
    await db`UPDATE sys_role SET permissions='["system:dept:query"]'::jsonb WHERE id=${role}`;
    await db`INSERT INTO sys_user_role(id,user_id,role_id) VALUES(${crypto.randomUUID()},${user},${role})`;
    stream = await open('/v1/departments?page=1&pageSize=20', user);
    await stream.next();
    await db`UPDATE sys_role SET permissions='[]'::jsonb WHERE id=${role}`;
    const denied = await stream.next('error');
    assert.equal(denied.code, 11007);
    console.log('PASS active subscription closes after role permission revocation');
} finally {
    await stream?.close();
    await db`DELETE FROM sys_user_role WHERE user_id=${user}`;
    await db`DELETE FROM sys_user WHERE id=${user}`;
    await db`DELETE FROM sys_role WHERE id=${role}`;
    await db`DELETE FROM sys_department WHERE id=${department}`;
    await db.close();
}
