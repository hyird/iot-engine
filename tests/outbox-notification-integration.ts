import { databaseUrl, apiBase } from './architecture-fixture';
// Only the disposable architecture fixture, never a deployed database.
import assert from 'node:assert/strict';

const db = new Bun.SQL(databaseUrl);
const ids: string[] = [];
async function until(check: () => Promise<boolean>, message: string, timeout = 10000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (await check()) return;
        await Bun.sleep(50);
    }
    throw new Error(message);
}
async function published(id: string) {
    const rows = await db`SELECT published_at IS NOT NULL AS done FROM outbox_event WHERE id=${id}`;
    return rows[0]?.done === true;
}
async function insert(delaySeconds = 0) {
    const id = crypto.randomUUID();
    ids.push(id);
    await db`INSERT INTO outbox_event(id,event_type,aggregate_type,aggregate_id,action,schema_version,available_at)
        VALUES(${id},'query.changed','auth',${id},'test',1,clock_timestamp()+make_interval(secs=>${delaySeconds}))`;
    return id;
}
async function checks() {
    const response = await fetch(apiBase + '/internal/metrics', { signal: AbortSignal.timeout(5000) });
    assert.equal(response.status, 200);
    const text = await response.text();
    const match = text.match(/^iot_engine_outbox_dispatch_checks_total\s+(\d+)/m);
    assert(match, 'outbox dispatch counter is missing');
    return Number(match[1]);
}

try {
    await until(async () => {
        const rows = await db`SELECT count(*)::int AS n FROM pg_stat_activity
            WHERE datname=current_database() AND application_name='iot-engine-outbox-listener' AND state='idle'`;
        return rows[0].n >= 1;
    }, 'LISTEN connection not established');
    await until(async () => (await db`SELECT count(*)::int AS n FROM outbox_event
        WHERE published_at IS NULL AND dead_lettered_at IS NULL`)[0].n === 0, 'fixture outbox did not drain');
    // Statistics timers still run, but an empty queue must not cause dispatch SQL.
    await Bun.sleep(500);
    const idle = await checks();
    await Bun.sleep(1500);
    assert.equal(await checks(), idle, 'empty outbox is still periodically dispatched');
    await db`UPDATE sys_department SET name=name WHERE id=${crypto.randomUUID()}`;
    await Bun.sleep(300);
    assert.equal(await checks(), idle, 'zero-row UPDATE emitted a false query change');
    console.log('PASS empty queue does not poll for dispatch work');

    const rolledBack = crypto.randomUUID();
    const rollback = new Error('intentional rollback');
    try {
        await db.begin(async (tx) => {
            await tx`INSERT INTO outbox_event(id,event_type,aggregate_type,aggregate_id,action,schema_version)
                VALUES(${rolledBack},'query.changed','auth',${rolledBack},'test',1)`;
            await Bun.sleep(500);
            assert.equal(await checks(), idle, 'uncommitted INSERT woke dispatch');
            throw rollback;
        });
    } catch (error) { if (error !== rollback) throw error; }
    assert.equal((await db`SELECT id FROM outbox_event WHERE id=${rolledBack}`).length, 0);
    const immediate = await insert();
    await until(() => published(immediate), 'committed event was not dispatched');
    console.log('PASS commit wakes dispatch; rollback does not publish');

    const future = await insert(2);
    await Bun.sleep(500);
    assert.equal(await published(future), false, 'future event published before its deadline');
    await until(() => published(future), 'future event stranded without a new notification');
    console.log('PASS durable future deadline wakes dispatch without another insert');

    const locked = await insert(1);
    await db.begin(async (tx) => {
        await tx`SELECT id FROM outbox_event WHERE id=${locked} FOR UPDATE`;
        await Bun.sleep(1500);
        assert.equal(await published(locked), false);
    });
    await until(() => published(locked), 'row skipped while locked was stranded after unlock');
    console.log('PASS concurrent row lock release does not strand pending work');

    const old = await db`SELECT pid FROM pg_stat_activity WHERE datname=current_database()
        AND application_name='iot-engine-outbox-listener'`;
    assert(old.length > 0);
    await db`SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname=current_database()
        AND application_name='iot-engine-outbox-listener'`;
    const recovering = await insert();
    await until(() => published(recovering), 'reconnected listener did not catch up durable work', 15000);
    await until(async () => {
        const now = await db`SELECT pid FROM pg_stat_activity WHERE datname=current_database()
            AND application_name='iot-engine-outbox-listener' AND state='idle'`;
        return now.length >= old.length && now.every((row: { pid: number }) =>
            !old.some((previous: { pid: number }) => previous.pid === row.pid));
    }, 'LISTEN sessions did not reconnect', 15000);
    console.log('PASS listener disconnect/reconnect catches up committed events');
} finally {
    if (ids.length) await db`DELETE FROM outbox_event WHERE id IN ${db(ids)}`;
    await db.close();
}
