import { expect, test } from 'bun:test';
import { readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';

async function files(directory: string): Promise<string[]> {
    const entries = await readdir(directory, { withFileTypes: true });
    return (await Promise.all(entries.map((entry) => entry.isDirectory()
        ? files(join(directory, entry.name)) : [join(directory, entry.name)]))).flat();
}

test('business query routes have no JSON GET fallback', async () => {
    const plain: string[] = [];
    for (const path of await files('service/modules')) {
        if (!path.endsWith('.controller.h')) continue;
        const source = await readFile(path, 'utf8');
        for (const match of source.matchAll(/RUVIA_GET\("([^"]+)"/g))
            plain.push(`${path.replaceAll('\\', '/')}:${match[1]}`);
        expect(source).not.toContain('/realtime/events');
        expect(source).not.toContain('/commands/wait');
    }
    expect(plain.sort()).toEqual([
        'service/modules/edge_node/edge_node.controller.h:/:id/download',
        'service/modules/system/operations/operations.controller.h:/health/live',
        'service/modules/system/operations/operations.controller.h:/health/ready',
        'service/modules/system/operations/operations.controller.h:/metrics',
    ]);
});

test('business clients cannot use JSON GET or interval refetch', async () => {
    for (const path of await files('web/pages')) {
        if (!/\.[jt]sx?$/.test(path)) continue;
        const source = await readFile(path, 'utf8');
        expect(source).not.toMatch(/\brequest\.get\s*[<(]/);
        expect(source).not.toMatch(/refetchInterval\s*:/);
        expect(source).not.toContain('subscribeDeviceRealtime');
    }
});
