import { expect, test } from 'bun:test';
import { edgeGroupView } from '../web/pages/iot/edge-node/edge-node.groups';
import type { Edge } from '../web/pages/iot/edge-node/edge-node.types';

const groups = [{ id: 'parent', name: '父组', children: [{ id: 'child', name: '子组', children: [] }] }] as Edge.GroupTreeItem[];
const node = (id: string, groupId: string, online: boolean) => ({ id, groupId, name: id, imei: id, enrollmentStatus: 'approved', status: { online } }) as Edge.Node;
const nodes = [node('parent-node', 'parent', true), node('child-node', 'child', false), node('loose-node', '', true)];

test('parent selection includes descendants once and aggregates their status', () => {
    const result = edgeGroupView(groups, nodes, 'parent', '');
    expect(result.filtered.map((item) => item.id)).toEqual(['parent-node', 'child-node']);
    expect(result.direct.get('parent')?.map((item) => item.id)).toEqual(['parent-node']);
    expect(result.stats.get('parent')).toEqual({ total: 2, online: 1, offline: 1 });
    expect(result.stats.get('child')).toEqual({ total: 1, online: 0, offline: 1 });
});

test('search retains the matching child hierarchy and ungrouped selection stays separate', () => {
    const searched = edgeGroupView(groups, nodes, null, ' CHILD ');
    expect(searched.filtered.map((item) => item.id)).toEqual(['child-node']);
    expect(searched.stats.get('parent')?.total).toBe(1);
    expect(searched.ungroupedCount).toBe(1);
    const ungrouped = edgeGroupView(groups, nodes, 'ungrouped', '');
    expect(ungrouped.roots).toEqual([]);
    expect(ungrouped.filtered.map((item) => item.id)).toEqual(['loose-node']);
});

test('registration filter and deleted selection do not leak nodes from other groups', () => {
    expect(edgeGroupView(groups, nodes, null, '', 'pending').filtered).toEqual([]);
    expect(edgeGroupView(groups, nodes, 'deleted', '').filtered).toEqual([]);
});
