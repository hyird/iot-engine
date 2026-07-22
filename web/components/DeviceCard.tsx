import { Tooltip } from 'antd';
import { memo, type ReactNode, useMemo } from 'react';

export interface DeviceCardItem {
    key: string | number;
    label: string;
    children: ReactNode;
    span?: number;
    group?: string;
    tooltipLabel?: string;
}

interface DeviceCardSection {
    key: string;
    label: string;
    items: DeviceCardItem[];
}

interface DeviceCardProps {
    title: ReactNode;
    subtitle?: ReactNode;
    items: DeviceCardItem[];
    column?: number;
    extra?: ReactNode;
}

const UNGROUPED_GROUP_KEY = '__ungrouped__';
const AUTO_LAYOUT_COLUMNS = 12;

const weightedLength = (value: string) =>
    Array.from(value).reduce(
        (length, character) => length + ((character.codePointAt(0) ?? 0) > 0xff ? 2 : 1),
        0
    );

const buildSections = (items: DeviceCardItem[]): DeviceCardSection[] => {
    const sections = new Map<string, DeviceCardSection>();
    for (const item of items) {
        const group = item.group?.trim();
        const key = group || UNGROUPED_GROUP_KEY;
        const current = sections.get(key);
        if (current) current.items.push(item);
        else sections.set(key, { key, label: group || '未分组', items: [item] });
    }
    return [...sections.values()];
};

const compactGroupedItemLabel = (groupLabel: string, item: DeviceCardItem): DeviceCardItem => {
    const groupPrefix = groupLabel.match(/^\d+#/)?.[0];
    if (!groupPrefix || !item.label.startsWith(`${groupPrefix}闸`)) return item;
    return {
        ...item,
        label: item.label.slice(`${groupPrefix}闸`.length),
        tooltipLabel: item.label,
    };
};

const getAlignedSpan = (item: DeviceCardItem, wide: boolean, columnCount: number) => {
    if (item.span !== undefined) return Math.min(columnCount, Math.max(1, item.span));

    const value =
        typeof item.children === 'string' || typeof item.children === 'number'
            ? String(item.children)
            : '';
    const visualLength = Math.max(weightedLength(item.label), weightedLength(value) + 2) + 2.5;
    const rawSpan = Math.min(
        AUTO_LAYOUT_COLUMNS,
        Math.max(2, Math.ceil(visualLength / (wide ? 8 : 4)))
    );
    const alignedTrackSize = AUTO_LAYOUT_COLUMNS / columnCount;
    return Math.min(columnCount, Math.max(1, Math.ceil(rawSpan / alignedTrackSize)));
};

const buildAlignedLayout = (items: DeviceCardItem[], wide: boolean, columnCount: number) => {
    if (columnCount === 1) return items.map((item) => ({ item, span: 1 }));

    const layout: Array<{ item: DeviceCardItem; span: number }> = [];
    let pendingItem: DeviceCardItem | undefined;

    for (const item of items) {
        const span = getAlignedSpan(item, wide, columnCount);
        if (span >= columnCount) {
            if (pendingItem) {
                layout.push({ item: pendingItem, span: columnCount });
                pendingItem = undefined;
            }
            layout.push({ item, span: columnCount });
            continue;
        }

        if (pendingItem) {
            layout.push({ item: pendingItem, span: 1 }, { item, span: 1 });
            pendingItem = undefined;
        } else {
            pendingItem = item;
        }
    }

    if (pendingItem) layout.push({ item: pendingItem, span: columnCount });
    return layout;
};

const DeviceValues = ({
    items,
    wide = false,
    compact = false,
}: {
    items: DeviceCardItem[];
    wide?: boolean;
    compact?: boolean;
}) => {
    const columnCount = wide ? 2 : 1;
    const layoutItems = buildAlignedLayout(items, wide, columnCount);

    return (
        <div
            className="grid items-stretch gap-1.5"
            style={{ gridTemplateColumns: `repeat(${columnCount}, minmax(0, 1fr))` }}
        >
            {layoutItems.map(({ item, span }) => (
                <div
                    key={item.key}
                    className={`h-full min-w-0 rounded-md bg-white/80 px-1 py-1 leading-[18px] shadow-[inset_0_0_0_1px_rgba(148,163,184,0.12)] ${compact ? 'text-[12px]' : 'text-[13px]'}`}
                    style={{ gridColumn: `span ${span}` }}
                >
                    <Tooltip title={item.tooltipLabel ?? item.label}>
                        <div className="min-w-0 truncate text-center text-[10px] font-medium text-slate-500">
                            {item.label}
                        </div>
                    </Tooltip>
                    {typeof item.children === 'string' || typeof item.children === 'number' ? (
                        <Tooltip title={item.children}>
                            <div className="min-w-0 truncate whitespace-nowrap text-center font-semibold tabular-nums text-slate-950">
                                {String(item.children)}
                            </div>
                        </Tooltip>
                    ) : (
                        <div className="min-w-0 text-center font-semibold text-slate-950">
                            {item.children}
                        </div>
                    )}
                </div>
            ))}
        </div>
    );
};

const DeviceCard = ({ title, subtitle, items, column = 4, extra }: DeviceCardProps) => {
    const sections = useMemo(() => buildSections(items), [items]);
    const hasGroupSections = sections.some((section) => section.key !== UNGROUPED_GROUP_KEY);

    return (
        <div className="flex h-full flex-col gap-1.5 rounded-lg border border-slate-200 bg-white px-3.5 py-2.5 shadow-[0_8px_24px_rgba(15,23,42,0.08)] transition-shadow hover:shadow-[0_12px_28px_rgba(15,23,42,0.12)]">
            <div className="min-w-0 text-[15px] font-semibold leading-5 text-slate-950">
                {title}
            </div>
            {subtitle && <div className="text-xs leading-5 text-slate-500">{subtitle}</div>}
            <div className="h-px bg-slate-100" />
            <div
                className={
                    hasGroupSections
                        ? 'flex flex-1 flex-col gap-1'
                        : 'flex flex-1 flex-col justify-between gap-1'
                }
            >
                {hasGroupSections ? (
                    sections.map((section) => (
                        <section
                            key={section.key}
                            className="flex flex-col rounded-md border border-slate-100 bg-slate-50/70 px-2 py-1.5"
                        >
                            <div className="mb-1.5 flex shrink-0 items-center gap-2">
                                <span className="rounded-full bg-white px-2 py-0 text-[11px] font-semibold leading-5 text-slate-600 shadow-sm">
                                    {section.label}
                                </span>
                                <span className="h-px flex-1 bg-slate-100" />
                            </div>
                            <DeviceValues
                                items={section.items.map((item) =>
                                    compactGroupedItemLabel(section.label, item)
                                )}
                                wide={column >= 8}
                                compact={section.items.length >= 8}
                            />
                        </section>
                    ))
                ) : (
                    <DeviceValues items={items} wide={column >= 8} />
                )}
            </div>
            {extra && (
                <div className="mt-0.5 border-t border-slate-100 pt-1.5 text-[11px] leading-4 text-slate-500">
                    {extra}
                </div>
            )}
        </div>
    );
};

export default memo(DeviceCard);
