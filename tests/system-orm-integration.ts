import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { apiBase, databaseUrl } from './architecture-fixture';

// Run only against the local disposable fixture guarded by architecture-fixture.
const sql = new Bun.SQL(databaseUrl);
const admin = '00000000-0000-7000-8000-000000000002';
const tag = `orm_${crypto.randomUUID().replaceAll('-', '').slice(0, 12)}`;
const encode = (value: unknown) => Buffer.from(JSON.stringify(value)).toString('base64url');
const now = Math.floor(Date.now() / 1000);
const unsigned = `${encode({ alg: 'HS256', typ: 'JWT' })}.${encode({
    iss: 'iot-engine', aud: 'iot-engine-web', sub: admin, user_id: admin,
    username: 'admin', token_type: 'access', iat: now, exp: now + 3600,
})}`;
const token = `${unsigned}.${createHmac('sha256', 'architecture-test-only-access-secret-000000000').update(unsigned).digest('base64url')}`;
const headers = { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' };

async function write(method: string, path: string, body?: unknown, status = 200) {
    const response = await fetch(apiBase + path, {
        method, headers, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(10000),
    });
    const text = await response.text();
    assert.equal(response.status, status, `${method} ${path}: ${text}`);
    return JSON.parse(text);
}

async function snapshot(path: string) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 10000);
    try {
        const response = await fetch(apiBase + path, { headers: { ...headers, Accept: 'text/event-stream' }, signal: controller.signal });
        assert.equal(response.status, 200, `${path}: ${response.status === 200 ? '' : await response.text()}`);
        assert(response.body);
        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        let text = '';
        try {
            for (;;) {
                const part = await reader.read();
                assert(!part.done, 'SSE ended before its initial snapshot');
                text += decoder.decode(part.value, { stream: true });
                const event = text.replaceAll('\r\n', '\n').split('\n\n')[0];
                if (text.replaceAll('\r\n', '\n').includes('\n\n') && event.includes('event: snapshot')) {
                    return JSON.parse(event.split('\n').filter(line => line.startsWith('data:')).map(line => line.slice(5).trimStart()).join('\n')).data;
                }
            }
        } finally {
            controller.abort();
            await reader.cancel().catch(() => {});
        }
    } finally { clearTimeout(timeout); controller.abort(); }
}

const roles: string[] = [], departments: string[] = [], users: string[] = [];
try {
    for (const suffix of ['a', 'b']) {
        const code = `${tag}_${suffix}`;
        await write('POST', '/v1/roles', { name: code, code, permissions: ['z:test', 'a:test'] });
        const [row] = await sql`SELECT id, description, permissions FROM sys_role WHERE code=${code}`;
        roles.push(row.id);
        assert.equal(row.description, null);
        assert.deepEqual(row.permissions, ['z:test', 'a:test']);
    }
    assert.deepEqual((await snapshot(`/v1/roles/${roles[0]}`)).permissions, ['a:test', 'z:test']);
    const page = await snapshot(`/v1/roles?keyword=${tag.toUpperCase()}&page=2&pageSize=1`);
    assert.equal(page.total, 2); assert.equal(page.list.length, 1); assert.equal(page.totalPages, 2);
    const empty = await snapshot(`/v1/roles?keyword=${tag}&page=9&pageSize=1`);
    assert.equal(empty.total, 2); assert.deepEqual(empty.list, []);
    assert.equal((await snapshot(`/v1/roles?keyword=${encodeURIComponent("' OR TRUE --")}`)).total, 0);
    await write('PUT', `/v1/roles/${roles[0]}`, { permissions: [], description: '' });
    assert.deepEqual((await snapshot(`/v1/roles/${roles[0]}`)).permissions, []);
    await write('PUT', `/v1/roles/${roles[0]}`, { permissions: ['bad,permission'] }, 400);
    assert.deepEqual((await snapshot(`/v1/roles/${roles[0]}`)).permissions, []);
    assert((await snapshot('/v1/roles/options')).some((row: { id: string }) => row.id === roles[0]));
    console.log('PASS role permissions, parameter binding, options and empty-page count');

    await write('POST', '/v1/departments', { name: `${tag}_root`, code: `${tag}_root` });
    const [root] = await sql`SELECT id, parent_id, leader_id FROM sys_department WHERE code=${`${tag}_root`}`;
    departments.push(root.id);
    assert.equal(root.parent_id, null); assert.equal(root.leader_id, null);
    await write('POST', '/v1/departments', { name: `${tag}_child`, code: `${tag}_child`, parent_id: root.id, leader_id: admin, sort_order: 3 });
    const [child] = await sql`SELECT id FROM sys_department WHERE code=${`${tag}_child`}`;
    departments.push(child.id);
    const detail = await snapshot(`/v1/departments/${child.id}`);
    assert.equal(detail.parent_name, `${tag}_root`); assert.equal(detail.leader_id, admin);
    assert.match(detail.created_at, /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/);
    assert.equal((await snapshot(`/v1/departments?parent_id=${root.id}&keyword=${tag.toUpperCase()}`)).total, 1);
    assert.equal((await snapshot(`/v1/departments?parent_id=&keyword=${tag}`)).total, 1);
    await write('PUT', `/v1/departments/${root.id}`, { parent_id: child.id }, 400);
    await write('DELETE', `/v1/departments/${root.id}`, undefined, 409);
    await write('PUT', `/v1/departments/${child.id}`, { parent_id: '', leader_id: '', code: '', status: 'disabled' });
    const [cleared] = await sql`SELECT parent_id, leader_id, code FROM sys_department WHERE id=${child.id}`;
    assert.equal(cleared.parent_id, null); assert.equal(cleared.leader_id, null); assert.equal(cleared.code, null);
    assert((await snapshot('/v1/departments/options')).some((row: { id: string }) => row.id === child.id));
    console.log('PASS department joins, UTC timestamps, nullable fields and recursive cycle guard');

    await write('POST', '/v1/users', { username: tag, password: 'orm-test-only-password', department_id: root.id, role_ids: roles });
    const [user] = await sql`SELECT id, nickname, phone, email FROM sys_user WHERE username=${tag}`;
    users.push(user.id);
    assert.equal(user.nickname, null); assert.equal(user.phone, null); assert.equal(user.email, null);
    assert.equal((await snapshot(`/v1/users/${user.id}`)).roles.length, 2);
    assert.equal((await snapshot(`/v1/users?keyword=${tag.toUpperCase()}&status=enabled`)).total, 1);
    assert.equal((await snapshot(`/v1/users/options?keyword=${tag}`)).length, 1);
    await write('DELETE', `/v1/roles/${roles[0]}`, undefined, 409);
    await write('PUT', `/v1/users/${user.id}`, { role_ids: [roles[0], roles[0]] }, 400);
    assert.equal((await snapshot(`/v1/users/${user.id}`)).roles.length, 2);
    await write('PUT', `/v1/users/${user.id}`, { role_ids: [roles[1]], department_id: '', nickname: '', status: 'disabled' });
    const [updated] = await sql`SELECT department_id, nickname FROM sys_user WHERE id=${user.id}`;
    assert.equal(updated.department_id, null); assert.equal(updated.nickname, '');
    assert.equal((await snapshot(`/v1/users/${user.id}`)).roles.length, 1);
    assert.equal((await snapshot(`/v1/users/options?keyword=${tag}`)).length, 0);

    // Force a database failure after the user update / membership deletion to
    // verify that all three writes share the original transaction.
    await sql.unsafe(`CREATE FUNCTION orm_test_reject_binding() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'injected binding failure'; END $$`);
    await sql.unsafe('CREATE TRIGGER orm_test_reject_binding BEFORE INSERT ON sys_user_role FOR EACH ROW EXECUTE FUNCTION orm_test_reject_binding()');
    try {
        await write('PUT', `/v1/users/${user.id}`, { nickname: 'must roll back', role_ids: [roles[0]] }, 500);
        const [retained] = await sql`SELECT nickname FROM sys_user WHERE id=${user.id}`;
        assert.equal(retained.nickname, '');
        const membership = await sql`SELECT role_id FROM sys_user_role WHERE user_id=${user.id}`;
        assert.deepEqual(membership.map((row: { role_id: string }) => row.role_id), [roles[1]]);
    } finally {
        await sql.unsafe('DROP TRIGGER orm_test_reject_binding ON sys_user_role');
        await sql.unsafe('DROP FUNCTION orm_test_reject_binding()');
    }
    await write('DELETE', `/v1/users/${user.id}`);
    assert.equal((await snapshot(`/v1/users?keyword=${tag}`)).total, 0);
    await write('DELETE', `/v1/roles/${roles[1]}`);
    await write('POST', '/v1/roles', { name: 'duplicate deleted code', code: `${tag}_b` }, 409);
    await write('DELETE', `/v1/departments/${root.id}`);
    await write('POST', '/v1/departments', { name: 'duplicate deleted code', code: `${tag}_root` }, 409);
    console.log('PASS user role replacement, database-failure rollback, soft deletion and reserved codes');
} finally {
    for (const id of users) { await sql`DELETE FROM sys_user_role WHERE user_id=${id}`; await sql`DELETE FROM sys_user WHERE id=${id}`; }
    for (const id of roles) await sql`DELETE FROM sys_role WHERE id=${id}`;
    for (const id of departments.reverse()) await sql`DELETE FROM sys_department WHERE id=${id}`;
    await sql.close();
}
