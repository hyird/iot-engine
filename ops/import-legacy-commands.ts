// One-time cutover tool. Stop all old/new platform processes before --apply --offline.
// It preserves available old command history without replaying physical operations.
const apply = Bun.argv.includes('--apply');
if (apply && !Bun.argv.includes('--offline')) throw new Error('--apply requires --offline');
if (!Bun.env.DATABASE_URL || !Bun.env.REDIS_URL) throw new Error('DATABASE_URL and REDIS_URL are required');
const db = new Bun.SQL(Bun.env.DATABASE_URL);
const redis = new Bun.RedisClient(Bun.env.REDIS_URL);
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
let found = 0, imported = 0, invalid = 0;
try {
    let cursor = '0';
    do {
        const page = await redis.send('SCAN', [cursor, 'MATCH', 'iot:state:command:*', 'COUNT', '200']) as [string, string[]];
        cursor = page[0];
        for (const key of page[1]) {
            const fields = await redis.send('HGETALL', [key]) as string[] | Record<string, string>;
            const old: Record<string, string> = Array.isArray(fields) ? {} : fields;
            if (Array.isArray(fields))
                for (let index = 0; index + 1 < fields.length; index += 2) old[fields[index]] = fields[index + 1];
            const id = key.slice('iot:state:command:'.length);
            if (!uuid.test(id) || !uuid.test(old.device_id ?? '')) { invalid++; continue; }
            found++;
            if (!apply) continue;
            await db.begin(async (tx) => {
                const exists = await tx`SELECT id FROM device WHERE id=${old.device_id}::uuid`;
                if (!exists.length) { invalid++; return; }
                // FAILED in the old model mixed timeouts and confirmed failures.
                const status = old.status === 'SUCCESS' ? 'SUCCEEDED' : 'UNKNOWN';
                const at = Number(old.created_at_ms);
                const createdAt = Number.isFinite(at) && at > 0 ? new Date(at) : new Date();
                await tx`INSERT INTO command_request(id,actor,idempotency_key,device_id,payload,created_at)
                    VALUES(${id}::uuid,'legacy-import',${id}::uuid,${old.device_id}::uuid,${old}::jsonb,${createdAt})
                    ON CONFLICT DO NOTHING`;
                const rows = await tx`INSERT INTO command_operation(id,request_id,ordinal,device_id,device_code,
                    protocol,status,reason,created_at,completed_at)
                    VALUES(${id}::uuid,${id}::uuid,0,${old.device_id}::uuid,${old.device_code ?? ''},
                    ${old.protocol ?? ''},${status},'imported_at_cutover; execution not replayed',${createdAt},NOW())
                    ON CONFLICT DO NOTHING RETURNING id`;
                imported += rows.length;
            });
        }
    } while (cursor !== '0');
    console.log(JSON.stringify({ mode: apply ? 'applied' : 'dry-run', found, imported, invalid }));
} finally {
    await db.close();
    redis.close();
}
