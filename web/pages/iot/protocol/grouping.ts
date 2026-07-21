export const normalizeGroupName = (group?: string) => group?.trim() || '';

export const UNGROUPED_GROUP_KEY = '__ungrouped__';

export const getGroupKey = (group?: string) => normalizeGroupName(group) || UNGROUPED_GROUP_KEY;

export interface GroupSection<T> {
    key: string;
    label: string;
    count: number;
    items: T[];
    firstIndex: number;
}

export const buildGroupSections = <T extends { group?: string }>(items: T[]): GroupSection<T>[] => {
    const sectionMap = new Map<string, GroupSection<T>>();

    items.forEach((item, index) => {
        const rawGroup = normalizeGroupName(item.group);
        const key = getGroupKey(item.group);
        const label = rawGroup || '未分组';
        const current = sectionMap.get(key);

        if (current) {
            current.count++;
            current.items.push(item);
            return;
        }

        sectionMap.set(key, {
            key,
            label,
            count: 1,
            items: [item],
            firstIndex: index,
        });
    });

    return Array.from(sectionMap.values()).sort((a, b) => {
        if (a.firstIndex !== b.firstIndex) return a.firstIndex - b.firstIndex;
        return a.label.localeCompare(b.label, 'zh-Hans-CN');
    });
};

export const sortSectionsByOrder = <T extends { key: string }>(
    sections: T[],
    orderedKeys: string[]
): T[] => {
    const orderIndex = new Map<string, number>(orderedKeys.map((key, index) => [key, index]));
    const originalIndex = new Map<string, number>(
        sections.map((section, index) => [section.key, index])
    );

    return [...sections].sort((left, right) => {
        const leftOrder = orderIndex.get(left.key);
        const rightOrder = orderIndex.get(right.key);

        if (leftOrder !== undefined && rightOrder !== undefined) {
            return leftOrder - rightOrder;
        }

        if (leftOrder !== undefined) return -1;
        if (rightOrder !== undefined) return 1;

        return (originalIndex.get(left.key) ?? 0) - (originalIndex.get(right.key) ?? 0);
    });
};

export const reorderItemsByGroupOrder = <T extends { group?: string }>(
    items: T[],
    orderedKeys: string[]
): T[] => {
    const groupMap = new Map<string, T[]>();
    const originalOrder: string[] = [];

    for (const item of items) {
        const key = getGroupKey(item.group);
        const bucket = groupMap.get(key);
        if (bucket) {
            bucket.push(item);
            continue;
        }

        groupMap.set(key, [item]);
        originalOrder.push(key);
    }

    const nextOrder = [
        ...orderedKeys.filter((key) => groupMap.has(key)),
        ...originalOrder.filter((key) => !orderedKeys.includes(key)),
    ];

    return nextOrder.flatMap((key) => groupMap.get(key) ?? []);
};

export const reorderItemsWithinGroupOrder = <T extends { id: string; group?: string }>(
    items: T[],
    groupKey: string,
    orderedIds: string[]
): T[] => {
    const sections = buildGroupSections(items);
    const orderedIdSet = new Set(orderedIds);

    return sections.flatMap((section) => {
        if (section.key !== groupKey) {
            return section.items;
        }

        const itemMap = new Map(section.items.map((item) => [item.id, item]));
        const nextItems: T[] = [];

        for (const id of orderedIds) {
            const item = itemMap.get(id);
            if (item) {
                nextItems.push(item);
            }
        }

        for (const item of section.items) {
            if (!orderedIdSet.has(item.id)) {
                nextItems.push(item);
            }
        }

        return nextItems;
    });
};
