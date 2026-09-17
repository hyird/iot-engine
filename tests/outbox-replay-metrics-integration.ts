import assert from 'node:assert/strict';
import { createHmac, randomUUID } from 'node:crypto';
import { apiBase, databaseUrl, redisUrl } from './architecture-fixture';
import { openSnapshotSubscription } from './sse-fixture';
let subscription: Awaited<ReturnType<typeof openSnapshotSubscription>> | undefined;

const db = new Bun.SQL(databaseUrl);
const redis = new Bun.RedisClient(redisUrl);
const ids: string[] = [];
const deniedUser = randomUUID();
const tag = randomUUID().replaceAll('-', '');
const trigger = `replay_failure_${tag}`;
let triggerInstalled = false;
let counterTriggerInstalled = false;
let writesPaused = false;
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const admin = '00000000-0000-7000-8000-000000000002';
const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({
    iss: 'iot-engine', aud: 'iot-engine-web', sub: admin, user_id: admin,
    username: 'admin', token_type: 'access', iat: now, exp: now + 3600,
})}`;
const jwt = `${unsigned}.${createHmac('sha256', 'architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;

async function replay(id: string) {
    const response = await fetch(`${apiBase}/v1/system/outbox/dead-letters/${id}/replay`, {
        method:'POST',headers:{Authorization:`Bearer ${jwt}`},signal:AbortSignal.timeout(15000),
    });
    const reply = await response.json();
    assert.equal(response.ok,reply.code === 0);
    return reply.code;
}
async function snapshot() {
    assert(subscription);
    let event = await subscription.next();
    while (event.event === 'heartbeat') event = await subscription.next();
    assert.equal(event.event,'snapshot');
    return JSON.parse(event.data).data;
}

async function insert() {
    const id = randomUUID();
    ids.push(id);
    await db`INSERT INTO outbox_event(id,event_type,aggregate_type,aggregate_id,action,schema_version,dead_lettered_at)
        VALUES(${id},'query.changed','auth',${id},'test',1,clock_timestamp())`;
    return id;
}

async function counter() {
    const response = await fetch(`${apiBase}/internal/metrics`, { signal: AbortSignal.timeout(8000) });
    assert.equal(response.status, 200);
    const text = await response.text();
    const samples = [...text.matchAll(/^iot_engine_outbox_dead_letter_replays_total\{worker="(\d+)"\} (\d+)$/gm)];
    assert.equal(new Set(samples.map(sample => sample[1])).size, samples.length,
        'a Worker must contribute only one replay counter sample');
    return samples.reduce((sum, sample) => sum + Number(sample[2]), 0);
}

async function waitCounter(expected: number) {
    const deadline = Date.now() + 25000;
    let actual = -1;
    while (Date.now() < deadline) {
        actual = await counter();
        if (actual === expected) return;
        await Bun.sleep(100);
    }
    assert.equal(actual, expected, 'published Worker replay counters did not converge');
}

try {
    const unauthorized = await fetch(`${apiBase}/v1/system/outbox/dead-letters`);
    assert.equal(unauthorized.status,401);
    assert.equal((await unauthorized.json()).code,11004);
    await db`INSERT INTO sys_user(id,username,password_hash) VALUES(${deniedUser},${deniedUser},'unused-test-password')`;
    const deniedUnsigned = `${encode({alg:'HS256',typ:'JWT'})}.${encode({iss:'iot-engine',aud:'iot-engine-web',sub:deniedUser,user_id:deniedUser,username:deniedUser,token_type:'access',iat:now,exp:now+3600})}`;
    const deniedToken = `${deniedUnsigned}.${createHmac('sha256','architecture-test-only-access-secret-000000000').update(deniedUnsigned).digest('base64url')}`;
    for (const [method,path,accept] of [
        ['GET','/v1/system/outbox/dead-letters','application/json'],
        ['GET','/v1/system/outbox/dead-letters/events','text/event-stream'],
        ['POST',`/v1/system/outbox/dead-letters/${randomUUID()}/replay`,'application/json'],
    ]) {
        const denied = await fetch(apiBase+path,{method,headers:{Authorization:`Bearer ${deniedToken}`,Accept:accept},signal:AbortSignal.timeout(15000)});
        assert.equal(denied.status,403);
        assert.equal((await denied.json()).code,11007);
    }
    assert.equal(await replay('invalid'),10001);
    subscription = await openSnapshotSubscription('/v1/system/outbox/dead-letters/events',jwt);
    assert(Array.isArray(await snapshot()));
    const insertedPush = snapshot();
    const initial = await counter();
    const first = await insert();
    assert((await insertedPush).some((entry: {id:string}) => entry.id === first));
    const listed = await fetch(`${apiBase}/v1/system/outbox/dead-letters`,{headers:{Authorization:`Bearer ${jwt}`},signal:AbortSignal.timeout(15000)});
    assert.equal(listed.status,200);
    assert((await listed.json()).data.some((entry: {id:string}) => entry.id === first));
    const replayedPush = snapshot();
    assert.equal(await replay(first), 0);
    assert(!(await replayedPush).some((entry: {id:string}) => entry.id === first));
    await subscription.close();
    await waitCounter(initial + 1);
    assert.equal(await replay(first), 10003);
    assert.equal(await replay(randomUUID()), 10003);
    await Bun.sleep(6000);
    assert.equal(await counter(), initial + 1, 'repeated or missing replay was counted');
    console.log('PASS committed replay counts once; repeated and missing replay do not count');

    const failed = await insert();
    // The generated identifier and UUID are test-owned; no user SQL is interpolated.
    await db.unsafe(`CREATE FUNCTION ${trigger}() RETURNS trigger LANGUAGE plpgsql AS $$
        BEGIN RAISE EXCEPTION 'injected replay write failure'; END $$;
        CREATE TRIGGER ${trigger} BEFORE UPDATE ON outbox_event FOR EACH ROW
        WHEN (OLD.id='${failed}'::uuid) EXECUTE FUNCTION ${trigger}();`);
    triggerInstalled = true;
    assert.equal(await replay(failed), 10004);
    assert.equal((await db`SELECT dead_lettered_at IS NOT NULL AS dead FROM outbox_event WHERE id=${failed}`)[0]?.dead, true);
    await Bun.sleep(6000);
    assert.equal(await counter(), initial + 1, 'failed database update was counted');
    await db.unsafe(`DROP TRIGGER ${trigger} ON outbox_event; DROP FUNCTION ${trigger}();`);
    triggerInstalled = false;
    console.log('PASS failed replay write preserves the dead letter and does not count');

    const counterFailure = await insert();
    await db.unsafe(`CREATE FUNCTION ${trigger}() RETURNS trigger LANGUAGE plpgsql AS $$
        BEGIN RAISE EXCEPTION 'injected replay counter failure'; END $$;
        CREATE TRIGGER ${trigger} BEFORE INSERT OR UPDATE ON outbox_replay_counter
        FOR EACH ROW EXECUTE FUNCTION ${trigger}();`);
    counterTriggerInstalled = true;
    assert.equal(await replay(counterFailure), 10004);
    assert.equal((await db`SELECT dead_lettered_at IS NOT NULL AS dead FROM outbox_event WHERE id=${counterFailure}`)[0]?.dead, true,
        'counter failure must roll back the replay update');
    await db.unsafe(`DROP TRIGGER ${trigger} ON outbox_replay_counter; DROP FUNCTION ${trigger}();`);
    counterTriggerInstalled = false;
    assert.equal(await counter(), initial + 1);
    console.log('PASS counter persistence failure rolls back the replay in the same transaction');

    const paused = await insert();
    await redis.send('CLIENT', ['PAUSE', '10000', 'WRITE']);
    writesPaused = true;
    // HTTP completion must not wait for Redis writes after the durable DB update.
    assert.equal(await replay(paused), 0);
    assert.equal((await db`SELECT dead_lettered_at IS NULL AS replayed FROM outbox_event WHERE id=${paused}`)[0]?.replayed, true);
    await redis.send('CLIENT', ['UNPAUSE']);
    writesPaused = false;
    await waitCounter(initial + 2);
    console.log('PASS Redis write pause does not turn committed replay into failure or lose its metric');

    const concurrent = await insert();
    const outcomes = await Promise.all([replay(concurrent), replay(concurrent)]);
    assert.deepEqual(outcomes.sort(), [0, 10003]);
    await waitCounter(initial + 3);
    await Bun.sleep(6000);
    assert.equal(await counter(), initial + 3, 'concurrent duplicate replay was counted twice');
    console.log('PASS concurrent replay commits and counts exactly once across Worker requests');
} finally {
    if (writesPaused) await redis.send('CLIENT', ['UNPAUSE']);
    if (triggerInstalled) await db.unsafe(`DROP TRIGGER IF EXISTS ${trigger} ON outbox_event; DROP FUNCTION IF EXISTS ${trigger}();`);
    if (counterTriggerInstalled) await db.unsafe(`DROP TRIGGER IF EXISTS ${trigger} ON outbox_replay_counter; DROP FUNCTION IF EXISTS ${trigger}();`);
    for (const id of ids) await db`DELETE FROM outbox_event WHERE id=${id}`;
    await subscription?.close();
    await db`DELETE FROM sys_user WHERE id=${deniedUser}`;
    await db.close();
    redis.close();
}
