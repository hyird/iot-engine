// Isolated cutover verification. Stop the test HTTP process before running.
import assert from 'node:assert/strict';
const url = 'postgres://architecture_test@127.0.0.1:55439/iot_architecture';
const redisUrl = 'redis://127.0.0.1:56439';
const db = new Bun.SQL(url); const redis = new Bun.RedisClient(redisUrl);
try {
 const devices = await db`SELECT id FROM device LIMIT 1`; const device = devices[0].id;
 const ids = [crypto.randomUUID(),crypto.randomUUID()];
 for (const [index,id] of ids.entries()) await redis.send('HSET',[`iot:state:command:${id}`,'device_id',device,'status',index ? 'FAILED':'SUCCESS','device_code','1']);
 for (const args of [[],['--apply','--offline'],['--apply','--offline']]) {
  const child = Bun.spawn(['bun','ops/import-legacy-commands.ts',...args],{env:{...Bun.env,DATABASE_URL:url,REDIS_URL:redisUrl},stdout:'pipe',stderr:'inherit'});
  const output = await new Response(child.stdout).text(); assert.equal(await child.exited,0); console.log(output.trim());
 }
 const rows=await db`SELECT status FROM command_operation WHERE id IN (${ids[0]}::uuid,${ids[1]}::uuid) ORDER BY status`;
 assert.deepEqual(rows.map(r=>r.status),['SUCCEEDED','UNKNOWN']);
 const attempts=await db`SELECT count(*)::int AS count FROM command_attempt WHERE operation_id IN (${ids[0]}::uuid,${ids[1]}::uuid)`; assert.equal(attempts[0].count,0);
 console.log('PASS legacy import dry-run, idempotency, ambiguous failure and no replay');
 const schema = await Bun.file('service/config/schema.h').text();
 const migration = schema.match(/0034_device_address_scope", R"sql\(([\s\S]*?)\)sql"/)![1];
 for (const duplicate of [false,true]) {
  let rejected = false;
  try { await db.begin(async tx => {
   await tx`CREATE TEMP TABLE protocol_config(id text PRIMARY KEY, protocol text) ON COMMIT DROP`;
   await tx`CREATE TEMP TABLE device(link_id text,protocol_config_id text,protocol_params jsonb,deleted_at timestamptz) ON COMMIT DROP`;
   await tx`CREATE UNIQUE INDEX idx_device_protocol_params_code ON device((protocol_params->>'device_code'))`;
   await tx`INSERT INTO protocol_config VALUES('sl','SL651')`;
   await tx`INSERT INTO device VALUES('link-a','sl','{"device_code":"1"}',NULL)`;
   const link = duplicate ? 'link-a':'link-b';
   await tx`INSERT INTO device VALUES(${link},'sl','{"device_code":"0000000001"}',NULL)`;
   await tx.unsafe(migration);
   const codes = await tx`SELECT protocol_params->>'device_code' AS code FROM device`;
   assert(codes.every(row => row.code === '0000000001'));
  }); } catch(error) { if ((error as {errno:string}).errno !== '23505') throw error; rejected=true; }
  assert.equal(rejected,duplicate);
 }
 console.log('PASS actual SL651 migration canonicalizes short addresses and rejects normalized same-link collisions');
} finally { await db.close();redis.close(); }
