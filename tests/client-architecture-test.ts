import { expect, test } from 'bun:test';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
const root = resolve(import.meta.dir, '..');
const client = join(root, 'clients/windows');
const walk = (dir: string): string[] => readdirSync(dir).flatMap(name => {
    const file = join(dir, name);
    return statSync(file).isDirectory() ? (name === 'vendor' ? [] : walk(file)) : [file];
});
const files = walk(client).filter(f => /\.(h|cpp)$/.test(f));
const paths = new Set(files);
const label = (p: string) => relative(root, p).replaceAll('\\', '/');
const edges = new Map<string, string[]>();
for (const file of files) {
    const dependencies: string[] = [];
    for (const match of readFileSync(file, 'utf8').matchAll(/^\s*#include\s+"([^"]+)"/gm)) {
        const target = [resolve(dirname(file), match[1]), resolve(client, 'common', match[1])].find(p => paths.has(p));
        if (target) dependencies.push(target);
    }
    edges.set(file, dependencies);
}
test('Windows UI and shared primitives never depend on the service implementation', () => {
    const violations: string[] = [];
    for (const file of files) {
        if (!/\/(common|winui)\//.test(label(file))) continue;
        const visited = new Set<string>();
        const visit = (node: string) => {
            if (visited.has(node)) return;
            visited.add(node);
            if (label(node).includes('/windows/service/')) violations.push(`${label(file)} -> ${label(node)}`);
            for (const target of edges.get(node) ?? []) visit(target);
        };
        visit(file);
    }
    expect(violations).toEqual([]);
});
test('Windows self-owned header dependencies are acyclic', () => {
    const active: string[] = [], visited = new Set<string>();
    const visit = (file: string) => {
        if (active.includes(file)) throw new Error([...active.slice(active.indexOf(file)), file].map(label).join(' -> '));
        if (visited.has(file)) return;
        active.push(file);
        for (const target of edges.get(file) ?? []) visit(target);
        active.pop(); visited.add(file);
    };
    for (const file of files) visit(file);
});
test('client build entry stays in CMake without PowerShell build scripts', () => {
    expect(walk(join(root, 'clients')).filter(file => file.endsWith('.ps1'))).toEqual([]);
});
