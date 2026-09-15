import { expect, test } from 'bun:test';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';

const root = resolve(import.meta.dir, '..');
const walk = (dir: string): string[] => readdirSync(dir).flatMap(name => {
    const file = join(dir, name);
    return statSync(file).isDirectory() ? walk(file) : [file];
});
const sources = walk(join(root, 'web')).filter(file => /\.tsx?$/.test(file));
const paths = new Set(sources);
const repoPath = (file: string) => relative(root, file).replaceAll('\\', '/');
const owner = (file: string) => file.includes('/pages/') ? dirname(file) : undefined;
const edges = new Map<string, string[]>();
for (const file of sources) {
    const imports: string[] = [];
    const content = readFileSync(file, 'utf8');
    for (const match of content.matchAll(/(?:\bfrom\s*|\bimport\s*\(\s*|\bimport\s*)['"]([^'"]+)['"]/g)) {
        const spec = match[1];
        if (!spec.startsWith('.') && !spec.startsWith('@/')) continue;
        const base = spec.startsWith('@/') ? resolve(root, 'web', spec.slice(2)) : resolve(dirname(file), spec);
        const target = [base, `${base}.ts`, `${base}.tsx`, join(base, 'index.ts'), join(base, 'index.tsx')].find(p => paths.has(p));
        if (target) imports.push(target);
    }
    edges.set(file, imports);
}

test('page modules use only their role files and have no private subdirectories', () => {
    const violations: string[] = [];
    const modules = new Set(sources.filter(f => /\/(?:index\.tsx|[^/]+\.service\.ts)$/.test(repoPath(f)) && repoPath(f).startsWith('web/pages/')).map(dirname));
    for (const dir of modules) {
        const name = dir.replaceAll('\\', '/').split('/').at(-1)!;
        for (const file of walk(dir)) {
            const local = relative(dir, file).replaceAll('\\', '/');
            if (!/^[a-z][a-z0-9_]*$/.test(name) || !['index.tsx', ...['types', 'schema', 'api', 'service'].map(role => `${name}.${role}.ts`)].includes(local)) violations.push(repoPath(file));
        }
    }
    expect(violations).toEqual([]);
});

test('shared frontend infrastructure never imports page implementations', () => {
    const violations: string[] = [];
    for (const [file, imports] of edges) {
        if (!/^web\/(components|store|hooks|utils|lib|types)\//.test(repoPath(file))) continue;
        for (const target of imports) if (repoPath(target).startsWith('web/pages/')) violations.push(`${repoPath(file)} -> ${repoPath(target)}`);
    }
    expect(violations).toEqual([]);
});

test('page dependencies use public services and types and views never import API clients', () => {
    const violations: string[] = [];
    for (const [file, imports] of edges) {
        if (!repoPath(file).startsWith('web/pages/')) continue;
        for (const target of imports) {
            if (!repoPath(target).startsWith('web/pages/')) continue;
            if ((owner(repoPath(file)) !== owner(repoPath(target)) && !/\.(service|types)\.ts$/.test(target)) || (file.endsWith('.tsx') && target.endsWith('.api.ts'))) violations.push(`${repoPath(file)} -> ${repoPath(target)}`);
        }
    }
    expect(violations).toEqual([]);
});

test('frontend source dependencies are acyclic', () => {
    const visited = new Set<string>();
    const active: string[] = [];
    const visit = (file: string) => {
        if (active.includes(file)) throw new Error([...active.slice(active.indexOf(file)), file].map(repoPath).join(' -> '));
        if (visited.has(file)) return;
        active.push(file);
        for (const target of edges.get(file) ?? []) visit(target);
        active.pop(); visited.add(file);
    };
    for (const file of sources) visit(file);
});

test('public page data types are defined in the owning types file', () => {
    const violations = sources.filter(file => repoPath(file).startsWith('web/pages/') && file.endsWith('.service.ts') && /^export\s+(?:interface|type)\s/m.test(readFileSync(file, 'utf8'))).map(repoPath);
    expect(violations).toEqual([]);
});

test('page views obtain network connections through their services', () => {
    const violations = sources.filter(file =>
        repoPath(file).startsWith('web/pages/') && file.endsWith('.tsx') &&
        /(?:\bnew\s+(?:WebSocket|EventSource)\s*\(|\bfetch\s*\()/.test(readFileSync(file, 'utf8'))
    ).map(repoPath);
    expect(violations).toEqual([]);
});
