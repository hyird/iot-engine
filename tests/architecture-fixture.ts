// Explicitly isolated endpoints; overrides allow concurrent local test runs.
function localUrl(value: string, protocol: string, database?: string) {
    const url = new URL(value);
    if (url.protocol !== protocol || !['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname)
        || (database && url.pathname !== `/${database}`)) {
        throw new Error('Architecture integration tests require a local disposable fixture');
    }
    return value;
}

export const databaseUrl = localUrl(Bun.env.ARCHITECTURE_DATABASE_URL ??
    'postgres://architecture_test@127.0.0.1:55439/iot_architecture', 'postgres:', 'iot_architecture');
export const redisUrl = localUrl(Bun.env.ARCHITECTURE_REDIS_URL ??
    'redis://127.0.0.1:56439', 'redis:');
export const apiBase = localUrl(Bun.env.TEST_BASE_URL ?? 'http://127.0.0.1:55102', 'http:');

// Synthetic integration inputs obey the same durable-entry + wake contract as
// the real producers. Do not rely on the recovery timer to make tests progress.
export async function publishFixtureEvent(redis: Bun.RedisClient, stream: string,
    fields: string[], task: string) {
    const wakes = await redis.send('KEYS', ['iot:service:worker:*:wake']) as string[];
    if (!wakes.length) throw new Error('Start disposable API workers before publishing fixture input');
    return redis.send('EVAL', [
        `local id=redis.call('XADD',KEYS[1],'*',unpack(ARGV,2));
         for i=2,#KEYS do redis.call('XADD',KEYS[i],'*','task',ARGV[1]) end;
         return id`,
        String(wakes.length + 1), stream, ...wakes, task, ...fields]);
}
