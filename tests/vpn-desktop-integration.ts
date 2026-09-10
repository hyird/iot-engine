import assert from 'node:assert/strict';
import { createHash, pbkdf2Sync, randomBytes, randomUUID } from 'node:crypto';
import { resolve } from 'node:path';

// Run with Run-VpnDesktopIntegration.ps1. Refuse to mutate another database.
const fixture = resolve(process.env.VPN_DESKTOP_FIXTURE ?? 'build/vpn-desktop-fixture');
const api = 'http://127.0.0.1:55112';
const db = new Bun.SQL('postgres://architecture_test@127.0.0.1:55449/iot_architecture');
const presence = new Bun.RedisClient('redis://127.0.0.1:56449');
const platform = '00000000-0000-7000-8000-000000000001';
const admin = '00000000-0000-7000-8000-000000000002';
const network = '00000000-0000-7000-8000-000000000004';
const edgeA = randomUUID(), edgeB = randomUUID(), routeA = randomUUID();
const roleVpn = randomUUID(), roleEdge = randomUUID(), user = randomUUID(), other = randomUUID();
const suffix = randomBytes(4).toString('hex');
const username = `vpn-${suffix}`, othername = `other-${suffix}`;
const password = 'DisposableVpnTest123!';
const controllers: AbortController[] = [];
let seeded = false;
let token = '', otherToken = '';
function check(response: { status: number, body: any }, status = 200) {
    assert.equal(response.status, status, JSON.stringify(response.body));
    return response.body.data;
}
async function request(path: string, method = 'GET', payload?: unknown, bearer = token) {
    const response = await fetch(api + path, { method, headers: {
        Accept: 'application/json', 'Content-Type': 'application/json', ...(bearer ? { Authorization: `Bearer ${bearer}` } : {})
    }, ...(payload === undefined ? {} : { body: JSON.stringify(payload) }), signal: AbortSignal.timeout(10000) });
    const text = await response.text();
    return { status: response.status, body: text ? JSON.parse(text) : null };
}
async function openStream(path: string, bearer = token, expectedStatus = 200) {
    const controller = new AbortController(); controllers.push(controller);
    const response = await fetch(api + path, { headers: { Accept: 'text/event-stream', ...(bearer ? { Authorization: `Bearer ${bearer}` } : {}) }, signal: controller.signal });
    if (response.status !== expectedStatus) throw Error(`SSE ${path}: ${response.status}, ${await response.text()}`);
    if (expectedStatus !== 200) { controller.abort(); return null!; }
    const reader = response.body!.getReader(); const decoder = new TextDecoder(); let buffer = '';
    return {
        close() { controller.abort(); },
        async next(predicate: (value: any) => boolean = () => true): Promise<any> {
            const timer = setTimeout(() => controller.abort(), 10000);
            try {
                for (;;) {
                    let end: number;
                    while ((end = buffer.indexOf('\n\n')) >= 0) {
                        const frame = buffer.slice(0, end); buffer = buffer.slice(end + 2);
                        const data = frame.split('\n').filter(line => line.startsWith('data:')).map(line => line.slice(5).trimStart()).join('\n');
                        if (!data || frame.includes('event: heartbeat')) continue;
                        const value = JSON.parse(data);
                        if (predicate(value)) return value;
                    }
                    const chunk = await reader.read(); if (chunk.done) throw Error('SSE ended before expected event');
                    buffer += decoder.decode(chunk.value, { stream: true }).replaceAll('\r\n', '\n');
                }
            } finally { clearTimeout(timer); }
        }
    };
}
const publicKey = () => randomBytes(32).toString('base64');
const enroll = (key: string, ids: unknown, bearer = token) => request('/v1/vpn/desktop/peers', 'POST', { name: 'Integration Windows', publicKey: key, edgeNodeIds: ids }, bearer);
const patch = (peer: string, ids: unknown, bearer = token) => request(`/v1/vpn/desktop/peers/${peer}`, 'PATCH', { edgeNodeIds: ids }, bearer);
try {
    const [location] = await db`SELECT current_setting('data_directory') AS path`;
    assert.equal(resolve(location.path).toLowerCase(), resolve(fixture, 'postgres').toLowerCase(), 'requires the disposable VPN fixture cluster');
    assert.equal((await db`SELECT to_regclass('public.vpn_effective_edge_access') AS name`)[0].name, 'vpn_effective_edge_access');
    const salt = randomBytes(16); const hash = `pbkdf2_sha256$210000$${salt.toString('hex')}$${pbkdf2Sync(password, salt, 210000, 32, 'sha256').toString('hex')}`;
    await db`INSERT INTO sys_role(id,code,name,status,permissions) VALUES
        (${roleVpn},${'vpn-'+suffix},'VPN','enabled','["iot:vpn:query","iot:vpn:enroll"]'::jsonb),
        (${roleEdge},${'edge-'+suffix},'Edge','enabled','["iot:edge:query"]'::jsonb)`;
    seeded = true;
    await db`INSERT INTO sys_user(id,username,password_hash,nickname,status) VALUES
        (${user},${username},${hash},'VPN test','enabled'),(${other},${othername},${hash},'Other test','enabled')`;
    for (const account of [user,other]) for (const role of [roleVpn,roleEdge])
        await db`INSERT INTO sys_user_role(id,user_id,role_id) VALUES (${randomUUID()},${account},${role})`;
    await db`UPDATE vpn_network SET hub_public_key=${publicKey()},hub_endpoint='127.0.0.1',hub_listen_port=51820,status='enabled' WHERE id=${network}`;
    for (const [edge,name,address] of [[edgeA,'Edge A','100.96.0.20'],[edgeB,'Edge B','100.96.0.21']]) {
        await db`INSERT INTO edge_node(id,platform_id,imei,name,model,enrollment_status,approved_by,approved_at) VALUES
            (${edge},${platform},${edge === edgeA ? '900000000000001' : '900000000000002'},${name},'test','approved',${admin},NOW())`;
        await db`INSERT INTO vpn_peer(id,network_id,peer_type,edge_node_id,name,public_key,assigned_ipv4,status) VALUES
            (${edge},${network},'edge',${edge},${name},${publicKey()},${address}::inet,'active')`;
    }
    await db`INSERT INTO vpn_route(id,network_id,edge_peer_id,lan_interface,target_cidr,virtual_cidr,created_by,status) VALUES
        (${routeA},${network},${edgeA},'lan','192.168.1.0/24','172.31.10.0/24',${admin},'active')`;
    await openStream('/v1/vpn/desktop/devices', '', 401);
    const login = check(await request('/v1/auth/login', 'POST', { username, password }, ''));
    assert.ok(login.token); assert.ok(login.refresh_token);
    const refreshed = check(await request('/v1/auth/refresh', 'POST', { refresh_token: login.refresh_token }, ''));
    token = refreshed.token; otherToken = check(await request('/v1/auth/login', 'POST', { username: othername, password }, '')).token;
    console.log('PASS actual login and refresh_token contract');
    const list = await openStream('/v1/vpn/desktop/devices'); const devices = (await list.next()).data;
    assert.equal(devices.filter((d: any) => [edgeA,edgeB].includes(d.id)).length, 2);
    assert.equal(devices.find((d: any) => d.id === edgeA).online, false); list.close();
    await db`UPDATE edge_node SET last_seen_at=NOW() - INTERVAL '1 day' WHERE id=${edgeA}`;
    const presenceStream=await openStream('/v1/vpn/desktop/devices'); await presenceStream.next();
    await presence.set(`iot:edge:session:${edgeA}`,'disposable-session');
    await presence.send('XADD', ['iot:live:changes', '*', 'topic', 'edge']);
    await presenceStream.next(x=>x.data?.find((d:any)=>d.id===edgeA)?.online===true);
    await db`UPDATE edge_node SET last_seen_at=NOW() WHERE id=${edgeA}`;
    await presence.del(`iot:edge:session:${edgeA}`);
    await presence.send('XADD', ['iot:live:changes', '*', 'topic', 'edge']);
    await presenceStream.next(x=>x.data?.find((d:any)=>d.id===edgeA)?.online===false);
    presenceStream.close();
    console.log('PASS desktop online follows live Edge session, independent of stale or fresh telemetry timestamp');
    assert.equal((await db`SELECT vpn_desktop_user_authorized(${user}::uuid) AS allowed`)[0].allowed, true);
    await db`INSERT INTO vpn_peer(id,network_id,peer_type,user_id,name,public_key,assigned_ipv4,status,client_managed)
        VALUES (${randomUUID()},${network},'windows',${user},'Previously revoked client',${publicKey()},'100.96.0.2'::inet,'revoked',TRUE)`;
    const key = publicKey(); const first = check(await enroll(key, [edgeA])); const peer = first.peerId;
    assert.equal(first.assignedIpv4,'100.96.0.2','historical revoked rows must no longer reserve an address');
    assert.deepEqual(first.edgeNodeIds, [edgeA]); assert.deepEqual(first.allowedRoutes.sort(), ['100.96.0.20/32','172.31.10.0/24']);
    assert.equal('privateKey' in first, false); assert.equal(first.publicKey, key);
    const retryKey = publicKey(); const retries = await Promise.all([enroll(retryKey,[edgeA]),enroll(retryKey,[edgeA])]);
    assert.equal(check(retries[0]).peerId,check(retries[1]).peerId);
    const allocated = await Promise.all([enroll(publicKey(),[edgeA]),enroll(publicKey(),[edgeB])]);
    assert.notEqual(check(allocated[0]).assignedIpv4,check(allocated[1]).assignedIpv4);
    const secondLogin = check(await request('/v1/auth/login','POST',{username,password},''));
    const secondToken = secondLogin.token;
    assert.ok(secondToken);
    const keyA=publicKey(), keyB=publicKey();
    const [computerA,computerB]=await Promise.all([enroll(keyA,[edgeA],token),enroll(keyB,[edgeA],secondToken)]).then(results=>results.map(result=>check(result)));
    assert.notEqual(computerA.peerId,computerB.peerId);
    assert.notEqual(computerA.assignedIpv4,computerB.assignedIpv4);
    const retriedA=check(await enroll(keyA,[edgeA],token));
    assert.equal(retriedA.peerId,computerA.peerId);
    assert.equal(retriedA.assignedIpv4,computerA.assignedIpv4);
    const streamA=await openStream(`/v1/vpn/desktop/peers/${computerA.peerId}/config`,token);
    assert.equal((await streamA.next()).data.assignedIpv4,computerA.assignedIpv4); streamA.close();
    check(await request(`/v1/vpn/desktop/peers/${computerA.peerId}`,'DELETE',undefined,token));
    const replacementA=check(await enroll(publicKey(),[edgeA],token));
    assert.equal(replacementA.assignedIpv4,computerA.assignedIpv4, 'revoked client address must be reused');
    assert.notEqual(replacementA.peerId,computerA.peerId);
    check(await request(`/v1/vpn/desktop/peers/${computerA.peerId}`,'DELETE',undefined,token));
    assert.equal((await db`SELECT status FROM vpn_peer WHERE id=${replacementA.peerId}`)[0].status,'active',
        'late disconnect for an old peer must not revoke its replacement');
    check(await request(`/v1/vpn/desktop/peers/${replacementA.peerId}`,'DELETE',undefined,token));
    check(await request('/v1/auth/logout','POST',undefined,token));
    const streamB=await openStream(`/v1/vpn/desktop/peers/${computerB.peerId}/config`,secondToken);
    assert.equal((await streamB.next()).data.assignedIpv4,computerB.assignedIpv4); streamB.close();
    check(await request(`/v1/vpn/desktop/peers/${computerB.peerId}`,'DELETE',undefined,secondToken));
    const recycled=await Promise.all([enroll(publicKey(),[edgeA],secondToken),enroll(publicKey(),[edgeB],secondToken)])
        .then(results=>results.map(result=>check(result)));
    assert.deepEqual(recycled.map(x=>x.assignedIpv4).sort(),[computerA.assignedIpv4,computerB.assignedIpv4].sort(),
        'concurrent connections must reuse the two released addresses without a collision');
    await assert.rejects(db`UPDATE vpn_peer SET status='active' WHERE id=${computerA.peerId}`,
        'database must reject reactivating an old peer whose address is now occupied');
    let cycling=recycled[0];
    for (let iteration=0; iteration<5; ++iteration) {
        check(await request(`/v1/vpn/desktop/peers/${cycling.peerId}`,'DELETE',undefined,secondToken));
        const next=check(await enroll(publicKey(),[edgeA],secondToken));
        assert.equal(next.assignedIpv4,cycling.assignedIpv4,'repeated disconnect/reconnect must not grow the address');
        cycling=next;
    }
    console.log('PASS historical revoked address reuse, concurrent reuse, stale disconnect fencing and repeated reconnect');
    const enrollmentId=randomUUID(), enrollmentToken=randomBytes(32).toString('hex'), enrollmentKey=publicKey();
    await db`INSERT INTO vpn_enrollment(id,token_hash,network_id,allowed_routes,expires_at,created_by)
        VALUES (${enrollmentId},${createHash('sha256').update(enrollmentToken).digest('hex')},${network},'[]'::jsonb,NOW()+INTERVAL '5 minutes',${user})`;
    const enrollBody={token:enrollmentToken,publicKey:enrollmentKey,name:'Token enrollment retry'};
    const tokenPeers=await Promise.all([
        request('/v1/vpn/client/enroll','POST',enrollBody,''),
        request('/v1/vpn/client/enroll','POST',enrollBody,'')
    ]).then(results=>results.map(result=>check(result)));
    assert.equal(tokenPeers[0].peerId,enrollmentId);
    assert.equal(tokenPeers[1].peerId,enrollmentId);
    assert.equal(tokenPeers[0].assignedIpv4,tokenPeers[1].assignedIpv4);
    check(await request('/v1/vpn/client/enroll','POST',{...enrollBody,publicKey:publicKey()},''),401);
    console.log('PASS token enrollment consumes and allocates atomically with same-key retry');
    console.log('PASS same account simultaneous logins, distinct client IPs, stable retry IP and independent logout');
    console.log('PASS selected routes, split-role permissions, concurrent enrollment and retry');
    for (const ids of [null,{},[1],['bad'],[edgeA,edgeA],[edgeA,edgeA.toUpperCase()],Array(65).fill(edgeA)]) check(await enroll(key,ids),400);
    for (const invalidKey of ['A'.repeat(43)+'=', 'H'.repeat(43)+'=', '='.repeat(44)]) check(await enroll(invalidKey,[edgeA]),400);
    check(await enroll(publicKey(),[randomUUID()]),400);
    check(await patch(peer,[edgeB],otherToken),404);
    await openStream(`/v1/vpn/desktop/peers/${peer}/config`,otherToken,404);
    check(await request(`/v1/vpn/desktop/peers/${peer}`,'DELETE',undefined,otherToken),404);
    assert.deepEqual(check(await patch(peer,[])).allowedRoutes,[]);
    assert.equal((await db`SELECT count(*)::int AS count FROM vpn_peer_edge_selection WHERE peer_id=${peer}`)[0].count,0);
    const updates = await Promise.all([patch(peer,[edgeA]),patch(peer,[edgeB])]); updates.forEach(x=>check(x));
    const selected = await db`SELECT edge_node_id FROM vpn_peer_edge_selection WHERE peer_id=${peer}`;
    assert.equal(selected.length,1); assert.ok([edgeA,edgeB].includes(selected[0].edge_node_id));
    check(await patch(peer,[edgeA]));
    const beforeKey=(await db`SELECT public_key FROM vpn_peer WHERE id=${peer}`)[0].public_key;
    await openStream(`/v1/vpn/client/config?peerId=${peer}`,token,404);
    assert.equal((await db`SELECT public_key FROM vpn_peer WHERE id=${peer}`)[0].public_key,beforeKey);
    console.log('PASS validation, ownership, empty selection, concurrent replacement and legacy rekey guard');
    const live=await openStream(`/v1/vpn/desktop/peers/${peer}/config`); await live.next();
    await db`UPDATE vpn_route SET virtual_cidr='172.31.11.0/24' WHERE id=${routeA}`;
    const changed=await live.next(x=>x.data?.allowedRoutes.includes('172.31.11.0/24'));
    assert.deepEqual(changed.data.edgeNodeIds,[edgeA]); assert.ok(!changed.data.allowedRoutes.includes('172.31.10.0/24'));
    await db`UPDATE vpn_route SET enabled=false WHERE id=${routeA}`;
    await live.next(x=>x.data?.allowedRoutes.length===1);
    await db`UPDATE edge_node SET enrollment_status='pending' WHERE id=${edgeA}`;
    await live.next(x=>x.data?.allowedRoutes.length===0);
    assert.equal(check(await enroll(key,[edgeA])).peerId,peer,'lost POST can recover identity after Edge approval changes');
    check(await patch(peer,[edgeA]),400);
    await db`UPDATE edge_node SET enrollment_status='approved' WHERE id=${edgeA}`;
    await db`UPDATE vpn_route SET enabled=true WHERE id=${routeA}`;
    await live.next(x=>x.data?.allowedRoutes.includes('172.31.11.0/24'));
    await db`UPDATE sys_role SET permissions='["iot:vpn:query"]'::jsonb WHERE id=${roleVpn}`;
    const revoked=await live.next(x=>x.code!==0); assert.equal(revoked.code,11007);
    assert.equal((await db`SELECT vpn_desktop_user_authorized(${user}::uuid) AS allowed`)[0].allowed,false);
    assert.equal((await db`SELECT count(*)::int AS count FROM vpn_effective_edge_access WHERE peer_id=${peer}`)[0].count,0);
    await openStream('/v1/vpn/desktop/devices',token,403);
    console.log('PASS one-stream route changes, Edge approval removal and enroll-permission revocation');
    await db`UPDATE sys_role SET permissions='["iot:vpn:query","iot:vpn:enroll"]'::jsonb WHERE id=${roleVpn}`;
    const pauseStream=await openStream(`/v1/vpn/desktop/peers/${peer}/config`,token); await pauseStream.next();
    await db`UPDATE vpn_network SET status='disabled' WHERE id=${network}`;
    const paused=await pauseStream.next(x=>x.data?.networkEnabled===false);
    assert.deepEqual(paused.data.allowedRoutes,[]); assert.deepEqual(paused.data.edgeAddresses,[]);
    const disabled=await openStream('/v1/vpn/desktop/devices'); assert.deepEqual((await disabled.next()).data,[]); disabled.close();
    check(await enroll(publicKey(),[edgeA]),404);
    await db`UPDATE vpn_network SET status='enabled' WHERE id=${network}`;
    await pauseStream.next(x=>x.data?.networkEnabled===true&&x.data.allowedRoutes.length>0); pauseStream.close();
    check(await request(`/v1/vpn/desktop/peers/${peer}`,'DELETE'));
    check(await request(`/v1/vpn/desktop/peers/${peer}`,'DELETE'));
    const revokedRegistration=await enroll(key,[edgeA]);
    check(revokedRegistration,410);
    assert.equal(revokedRegistration.body.code,21009,'only confirmed revocation clears a pending registration');
    await openStream(`/v1/vpn/desktop/peers/${peer}/config`,token,404);
    console.log('PASS disabled network and idempotent revocation');
    console.log('All desktop API integration groups passed');
} finally {
    await presence.del(`iot:edge:session:${edgeA}`); presence.close();
    for (const controller of controllers) controller.abort();
    if (seeded) {
        await db`UPDATE vpn_network SET status='enabled' WHERE id=${network}`;
        await db`DELETE FROM vpn_peer WHERE user_id IN (${user},${other}) OR id IN (${edgeA},${edgeB})`;
        await db`DELETE FROM vpn_enrollment WHERE created_by IN (${user},${other})`;
        await db`DELETE FROM edge_node WHERE id IN (${edgeA},${edgeB})`;
        await db`DELETE FROM sys_user WHERE id IN (${user},${other})`;
        await db`DELETE FROM sys_role WHERE id IN (${roleVpn},${roleEdge})`;
    }
    await db.close();
}
