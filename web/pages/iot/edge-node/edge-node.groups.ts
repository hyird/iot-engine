import type { Edge } from './edge-node.types';

export function edgeGroupView(
    groups: Edge.GroupTreeItem[],
    nodes: Edge.Node[],
    selectedId: string | null,
    keyword: string,
    status?: Edge.EnrollmentStatus
) {
    const index = new Map<string, Edge.GroupTreeItem>();
    const visit = (items: Edge.GroupTreeItem[]) =>
        items.forEach((group) => {
            index.set(group.id, group);
            visit(group.children ?? []);
        });
    visit(groups);
    const selected = selectedId ? index.get(selectedId) : undefined;
    const scope = new Set<string>();
    const collect = (group: Edge.GroupTreeItem) => {
        scope.add(group.id);
        group.children?.forEach(collect);
    };
    if (selected) collect(selected);
    const query = keyword.trim().toLowerCase();
    const filtered = nodes.filter((node) => {
        if (selectedId === 'ungrouped' && node.groupId) return false;
        if (selectedId && selectedId !== 'ungrouped' && !scope.has(node.groupId ?? ''))
            return false;
        if (status && node.enrollmentStatus !== status) return false;
        return (
            !query ||
            [node.name, node.imei, node.model, node.hostname].some((value) =>
                value?.toLowerCase().includes(query)
            )
        );
    });
    const direct = new Map<string, Edge.Node[]>();
    for (const node of filtered) {
        const id = node.groupId ?? '';
        const items = direct.get(id) ?? [];
        items.push(node);
        direct.set(id, items);
    }
    const stats = new Map<string, { total: number; online: number; offline: number }>();
    const count = (group: Edge.GroupTreeItem) => {
        const own = direct.get(group.id) ?? [];
        const value = {
            total: own.length,
            online: own.filter((node) => node.status.online).length,
            offline: 0,
        };
        for (const child of group.children ?? []) {
            const childStats = count(child);
            value.total += childStats.total;
            value.online += childStats.online;
        }
        value.offline = value.total - value.online;
        stats.set(group.id, value);
        return value;
    };
    groups.forEach(count);
    return {
        filtered,
        direct,
        stats,
        roots: selectedId === 'ungrouped' ? [] : selectedId ? (selected ? [selected] : []) : groups,
        ungrouped: filtered.filter((node) => !node.groupId),
        ungroupedCount: nodes.filter((node) => !node.groupId).length,
    };
}
