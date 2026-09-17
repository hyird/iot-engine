import { expect, spyOn, test } from 'bun:test';
import { buildEdgeNodeGroupView, queryEdgeSelectionList } from '../web/pages/iot/edge_node/edge_node.service';
import request from '../web/lib/http';
import { type Edge } from '../web/pages/iot/edge_node/edge_node.types';

const groups = [{ id: 'parent', name: '父组', children: [{ id: 'child', name: '子组', children: [] }] }] as Edge.GroupTreeItem[];
const node = (id: string, groupId: string, online: boolean) => ({ id, groupId, name: id, imei: id, enrollmentStatus: 'approved', status: { online } }) as Edge.Node;
const nodes = [node('parent-node', 'parent', true), node('child-node', 'child', false), node('loose-node', '', true)];

test('parent selection includes descendants once and aggregates their status', () => {
    const result = buildEdgeNodeGroupView(groups, nodes, 'parent', '');
    expect(result.filtered.map((item) => item.id)).toEqual(['parent-node', 'child-node']);
    expect(result.direct.get('parent')?.map((item) => item.id)).toEqual(['parent-node']);
    expect(result.stats.get('parent')).toEqual({ total: 2, online: 1, offline: 1 });
    expect(result.stats.get('child')).toEqual({ total: 1, online: 0, offline: 1 });
});

test('search retains the matching child hierarchy and ungrouped selection stays separate', () => {
    const searched = buildEdgeNodeGroupView(groups, nodes, null, ' CHILD ');
    expect(searched.filtered.map((item) => item.id)).toEqual(['child-node']);
    expect(searched.stats.get('parent')?.total).toBe(1);
    expect(searched.ungroupedCount).toBe(1);
    const ungrouped = buildEdgeNodeGroupView(groups, nodes, 'ungrouped', '');
    expect(ungrouped.roots).toEqual([]);
    expect(ungrouped.filtered.map((item) => item.id)).toEqual(['loose-node']);
});

test('registration filter and deleted selection do not leak nodes from other groups', () => {
    expect(buildEdgeNodeGroupView(groups, nodes, null, '', 'pending').filtered).toEqual([]);
    expect(buildEdgeNodeGroupView(groups, nodes, 'deleted', '').filtered).toEqual([]);
});

test('node selection loads every HTTP page once and passes cancellation to each request', async () => {
    const controller = new AbortController();
    const pages: number[] = [];
    const get = spyOn(request, 'get').mockImplementation(async (path, options) => {
        expect(path).toBe('/v1/edge');
        expect(options?.signal).toBe(controller.signal);
        const page = Number(options?.params?.page);
        pages.push(page);
        return { total: 201, list: [node(`node-${page}`, '', true), node('duplicate', '', false)] };
    });
    try {
        const result = await queryEdgeSelectionList(controller.signal);
        expect(pages).toEqual([1, 2, 3]);
        expect(result.map(item => item.id)).toEqual(['node-1', 'duplicate', 'node-2', 'node-3']);
        get.mockImplementation(async () => { throw new DOMException('Cancelled', 'AbortError'); });
        await expect(queryEdgeSelectionList(controller.signal)).rejects.toThrow('Cancelled');
    } finally { get.mockRestore(); }
});
