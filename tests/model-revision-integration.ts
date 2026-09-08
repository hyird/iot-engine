// Disposable local fixture only; all schema and data changes roll back.
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';

const db = new Bun.SQL('postgres://architecture_test@127.0.0.1:55439/iot_architecture');
const source = await Bun.file('service/config/model-revisions.h').text();
const migration = source.split('R"sql(')[1].split(')sql"')[0];
const sentinel = new Error('ROLLBACK_TEST');
try {
    await db.begin(async (tx) => {
        const installed = await tx`SELECT to_regclass('public.protocol_revision') AS relation`;
        if (!installed[0].relation) await tx.unsafe(migration);
        const id = randomUUID();
        await tx`INSERT INTO protocol_config(id,protocol,name,config,created_by)
          VALUES(${id},'Modbus',${`revision-test-${id}`},'{"registers":[],"storagePolicy":"report"}',
          '00000000-0000-7000-8000-000000000002')`;
        await tx`UPDATE protocol_config SET config='{"registers":[],"readInterval":2,"storagePolicy":"report"}' WHERE id=${id}`;
        const revisions = await tx`SELECT revision,config FROM protocol_revision WHERE id=${id} ORDER BY revision`;
        assert.equal(revisions.length, 2);
        assert.deepEqual(revisions[0].config, { registers: [], storagePolicy: "report" });
        assert.equal(revisions[1].config.readInterval, 2);
        await tx`UPDATE protocol_config SET enabled=false WHERE id=${id}`;
        const unchanged = await tx`SELECT revision FROM protocol_config WHERE id=${id}`;
        assert.equal(Number(unchanged[0].revision), 2);
        await tx.unsafe('SAVEPOINT immutable_test');
        await assert.rejects(tx`UPDATE protocol_revision SET name='mutated' WHERE id=${id}`,
            /immutable/);
        await tx.unsafe('ROLLBACK TO SAVEPOINT immutable_test');
        throw sentinel;
    });
} catch (error) {
    if (error !== sentinel) throw error;
    console.log('PASS model migration, immutable revisions, and unchanged-version behavior');
} finally { await db.close(); }
