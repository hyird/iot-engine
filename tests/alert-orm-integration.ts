import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { apiBase, databaseUrl, publishFixtureEvent, redisUrl } from './architecture-fixture';

const db = new Bun.SQL(databaseUrl);
const redis = new Bun.RedisClient(redisUrl);
const admin = '00000000-0000-7000-8000-000000000002';
const device = crypto.randomUUID();
const link = crypto.randomUUID();
const model = crypto.randomUUID();
const point = crypto.randomUUID();
const prefix = `orm-alert-${crypto.randomUUID()}`;
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({
    iss: 'iot-engine', aud: 'iot-engine-web', sub: admin, user_id: admin,
    username: 'admin', token_type: 'access', iat: now, exp: now + 3600,
})}`;
const token = `${unsigned}.${createHmac('sha256', 'architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;

async function until(check: () => Promise<boolean>, message: string) {
    const deadline = Date.now() + 20000;
    while (Date.now() < deadline) {
        if (await check()) return;
        await Bun.sleep(50);
    }
    throw new Error(message);
}

async function request(path: string, body?: unknown) {
    const response = await fetch(`${apiBase}/v1/alert${path}`, {
        method: body === undefined ? 'GET' : 'POST',
        headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json', Accept: 'text/event-stream' },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: AbortSignal.timeout(20000),
    });
    if (response.status !== 200) throw new Error(`${response.status}: ${(await response.text()).slice(0, 500)}`);
    if (response.headers.get('content-type')?.includes('text/event-stream')) {
        const reader = response.body!.getReader();
        const decoder = new TextDecoder();
        let text = '';
        try {
            for (;;) {
                const next = await reader.read();
                assert(!next.done, 'snapshot stream ended before its first result');
                text += decoder.decode(next.value, { stream: true });
                if (!text.includes('\n\n') && !text.includes('\r\n\r\n')) continue;
                const match = text.match(/^data: ?(.+)$/m);
                if (match) return JSON.parse(match[1]).data;
            }
        } finally {
            await reader.cancel();
        }
    }
    const result = await response.json();
    assert.equal(result.code, 0, JSON.stringify(result));
    return result.data;
}

async function rule(suffix: string, condition: object) {
    const name = `${prefix}-${suffix}`;
    await request('/rules', {
        name, device_id: device, severity: 'warning', conditions: [condition],
        silence_duration: 0, recovery_condition: 'reverse', recovery_wait_seconds: 0,
    });
    const rows = await db`SELECT id FROM alert_rule WHERE name=${name} AND device_id=${device}`;
    assert.equal(rows.length, 1);
    return rows[0].id as string;
}

async function publish(value: number, observedAt: number, messageId = crypto.randomUUID()) {
    const fields = [
        'message_id', messageId, 'causation_id', messageId, 'link_id', link,
        'device_id', device, 'device_code', '1', 'protocol', 'Modbus',
        'connection_id', device, 'occurred_at_ms', String(observedAt),
        'observed_at_ms', String(observedAt), 'model_id', model, 'model_revision', '1',
        'storage_policy', 'change', 'source', 'collector', 'online_window_ms', '60000',
        'values_json', JSON.stringify({ values: { [point]: { name: 'temperature', value } } }),
        'raw_payload_hex', '[]',
    ];
    await publishFixtureEvent(redis, 'iot:v3:telemetry', fields, 'telemetry');
    await until(async () => (await db`SELECT 1 FROM alert_evaluation_receipt WHERE message_id=${messageId}`).length === 1,
        `alert evaluation did not acknowledge ${messageId}`);
    return { messageId, fields };
}

try {
    await db`INSERT INTO protocol_config(id,name,protocol,config,created_by)
        VALUES(${model},${prefix},'Modbus',${{ registers: [{ id: point, name: 'temperature', registerType: 'HOLDING_REGISTER', dataType: 'UINT16', address: 0, quantity: 1 }], storagePolicy: 'change' }}::jsonb,${admin})`;
    await db`INSERT INTO link(id,name,protocol,endpoint,created_by,execution,status)
        VALUES(${link},${prefix},'Modbus','{"transport":"tcp","mode":"TCP Server","ip":"0.0.0.0","port":55209,"targets":[]}'::jsonb,${admin},'collector','disabled')`;
    await db`INSERT INTO device(id,name,link_id,protocol_config_id,protocol_revision,protocol_params,created_by)
        VALUES(${device},${prefix},${link},${model},1,'{"device_code":"1","slave_id":1,"modbus_mode":"TCP"}'::jsonb,${admin})`;
    const threshold = await rule('threshold', { type: 'threshold', elementKey: point, operator: '>', value: '50' });
    const rate = await rule('rate', { type: 'rate_of_change', elementKey: point, changeRate: '50', changeDirection: 'rise' });
    const bit = await rule('bit', { type: 'threshold', elementKey: point, operator: '==', value: '1', bitIndex: 0 });
    const timestamp = Date.now();
    await publish(10, timestamp);
    assert.equal((await db`SELECT id FROM open_alert_record WHERE device_id=${device}`).length, 0);
    const triggered = await publish(61, timestamp + 10);
    const records = await db`SELECT rule_id,status FROM open_alert_record WHERE device_id=${device}`;
    assert.deepEqual(new Set(records.map(row => row.rule_id)), new Set([threshold, rate, bit]));
    assert(records.every(row => row.status === 'active'));
    const replayId = String(await publishFixtureEvent(redis, 'iot:v3:telemetry:alerts', triggered.fields, 'telemetry-alerts'));
    await until(async () => (await redis.send('XRANGE', ['iot:v3:telemetry:alerts', replayId, replayId]) as unknown[]).length === 0,
        'duplicate alert input was not acknowledged');
    assert.equal((await db`SELECT id FROM open_alert_record WHERE device_id=${device}`).length, 3);
    await publish(20, timestamp + 20);
    assert((await db`SELECT status FROM open_alert_record WHERE device_id=${device}`).every(row => row.status === 'resolved'));
    const unchanged = await publish(20, timestamp + 30);
    await until(async () => (await db`SELECT record_id FROM device_latest_value WHERE device_id=${device} AND element_id=${point}`)[0]?.record_id === unchanged.messageId,
        'unchanged history input must still advance the latest read model');
    assert.equal(Number((await db`SELECT count(*) AS count FROM device_data WHERE device_id=${device}`)[0].count), 3,
        'change storage must persist changed samples once and skip unchanged samples');
    const latest = await db`SELECT value FROM device_latest_value WHERE device_id=${device} AND element_id=${point}`;
    assert.equal(latest[0].value.value, 20);
    const listing = await request(`/rules?keyword=${encodeURIComponent(prefix)}`);
    assert.equal(listing.total, 3);
    assert.equal(listing.list.length, 3);
    const stats = await request('/stats');
    assert.equal(typeof stats.total, 'number');
    const offline = await rule('offline', { type: 'offline', duration: 1 });
    await until(async () => (await db`SELECT id FROM open_alert_record WHERE rule_id=${offline} AND status='active'`).length === 1,
        'offline rule did not fire after its deadline');
    const templateName = `${prefix}-template`;
    await request('/templates', { name: templateName, protocol_config_id: model,
        conditions: [{ type: 'threshold', elementKey: point, operator: '>', value: '100' }] });
    const template = (await db`SELECT id FROM alert_rule_template WHERE name=${templateName}`)[0].id;
    const applied = await request('/rules/apply-template', { template_id: template, device_ids: [device] });
    assert.equal(applied.success, 1);
    assert.equal(applied.total, 1);
    assert.equal(applied.createdIds.length, 1);
    assert.equal((await request('/rules/apply-template', { template_id: template, device_ids: [device] })).success, 0);
    assert.equal((await request(`/templates/${template}`)).name, templateName);
    const grouped = await request('/records/grouped');
    assert(Array.isArray(grouped));
    console.log('PASS ORM alert thresholds, rates, bit conditions, recovery, receipts, JSON types and change storage');
} finally {
    await db`UPDATE alert_rule SET deleted_at=NOW() WHERE device_id=${device}`;
    await db`UPDATE alert_rule_template SET deleted_at=NOW() WHERE protocol_config_id=${model}`;
    await db`UPDATE device SET deleted_at=NOW() WHERE id=${device}`;
    await db`UPDATE link SET deleted_at=NOW() WHERE id=${link}`;
    await db`UPDATE protocol_config SET deleted_at=NOW() WHERE id=${model}`;
    await db.close();
    redis.close();
}
