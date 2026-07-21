import { HolderOutlined } from '@ant-design/icons';
import type { DragEndEvent } from '@dnd-kit/core';
import { closestCenter, DndContext, PointerSensor, useSensor, useSensors } from '@dnd-kit/core';
import {
    arrayMove,
    rectSortingStrategy,
    SortableContext,
    useSortable,
    verticalListSortingStrategy,
} from '@dnd-kit/sortable';
import { CSS } from '@dnd-kit/utilities';
import { Button, Empty, Flex, Space, Table } from 'antd';
import type { ColumnsType, TableProps } from 'antd/es/table';
import type { CSSProperties, ForwardedRef, HTMLAttributes, ReactNode } from 'react';
import {
    createContext,
    forwardRef,
    memo,
    useCallback,
    useContext,
    useEffect,
    useMemo,
    useState,
} from 'react';
import { sortSectionsByOrder } from './grouping';

interface SortableGroupSectionFrameProps {
    id: string;
    title: ReactNode;
    description?: ReactNode;
    meta?: ReactNode;
    actions?: ReactNode;
    children: ReactNode;
    disabled?: boolean;
    className?: string;
    bodyClassName?: string;
    style?: CSSProperties;
}

export const SortableGroupSectionFrame = memo(
    ({
        id,
        title,
        description,
        meta,
        actions,
        children,
        disabled = false,
        className,
        bodyClassName,
        style,
    }: SortableGroupSectionFrameProps) => {
        const { attributes, listeners, setNodeRef, transform, transition, isDragging } =
            useSortable({
                id,
                disabled,
            });

        return (
            <section
                ref={setNodeRef}
                className={className}
                style={{
                    transform: CSS.Transform.toString(transform),
                    transition: transition || undefined,
                    opacity: isDragging ? 0.75 : 1,
                    zIndex: isDragging ? 1 : undefined,
                    ...style,
                }}
            >
                <Flex justify="space-between" align="center" gap={12} wrap className="mb-3">
                    <div className="min-w-0">
                        <div className="text-sm font-semibold text-slate-800">{title}</div>
                        {description ? (
                            <div className="mt-1 text-xs text-slate-500">{description}</div>
                        ) : null}
                    </div>

                    <Space size={6} wrap>
                        {meta}
                        {actions}
                        <Button
                            type="text"
                            size="small"
                            icon={<HolderOutlined />}
                            disabled={disabled}
                            aria-label="拖拽排序"
                            title="拖拽排序"
                            className="!cursor-grab active:!cursor-grabbing"
                            style={{ touchAction: 'none' }}
                            {...attributes}
                            {...listeners}
                        />
                    </Space>
                </Flex>

                <div className={bodyClassName}>{children}</div>
            </section>
        );
    }
);

interface SortableGroupItemFrameProps {
    id: string;
    children: (dragHandle: ReactNode) => ReactNode;
    disabled?: boolean;
    className?: string;
    style?: CSSProperties;
}

export const SortableGroupItemFrame = memo(
    ({ id, children, disabled = false, className, style }: SortableGroupItemFrameProps) => {
        const { attributes, listeners, setNodeRef, transform, transition, isDragging } =
            useSortable({
                id,
                disabled,
            });

        const dragHandle = (
            <Button
                type="text"
                size="small"
                icon={<HolderOutlined />}
                disabled={disabled}
                aria-label="拖拽排序"
                title="拖拽排序"
                className="!cursor-grab active:!cursor-grabbing"
                style={{ touchAction: 'none' }}
                {...attributes}
                {...listeners}
            />
        );

        return (
            <div
                ref={setNodeRef}
                className={className}
                style={{
                    transform: CSS.Transform.toString(transform),
                    transition: transition || undefined,
                    opacity: isDragging ? 0.75 : 1,
                    zIndex: isDragging ? 1 : undefined,
                    ...style,
                }}
            >
                {children(dragHandle)}
            </div>
        );
    }
);

interface SortableGroupSectionListProps<T extends { key: string }> {
    sections: T[];
    children: (section: T) => ReactNode;
    empty?: ReactNode;
    className?: string;
    disabled?: boolean;
    onOrderChange?: (nextOrder: string[]) => void;
}

const areOrdersEqual = (left: string[], right: string[]) =>
    left.length === right.length && left.every((value, index) => value === right[index]);

const reconcileOrder = (previousOrder: string[], currentKeys: string[]) => {
    if (currentKeys.length === 0) return [];
    if (previousOrder.length === 0) return currentKeys;

    const currentKeySet = new Set(currentKeys);
    const filtered = previousOrder.filter((key) => currentKeySet.has(key));
    const missing = currentKeys.filter((key) => !filtered.includes(key));

    if (filtered.length === 0) return currentKeys;
    const nextOrder = [...filtered, ...missing];
    return areOrdersEqual(previousOrder, nextOrder) ? previousOrder : nextOrder;
};

export const SortableGroupSectionList = <T extends { key: string }>({
    sections,
    children,
    empty,
    className,
    disabled = false,
    onOrderChange,
}: SortableGroupSectionListProps<T>) => {
    const currentKeys = useMemo(() => sections.map((section) => section.key), [sections]);
    const [order, setOrder] = useState<string[]>(currentKeys);

    useEffect(() => {
        setOrder((previousOrder) => reconcileOrder(previousOrder, currentKeys));
    }, [currentKeys]);

    const orderedSections = useMemo(() => sortSectionsByOrder(sections, order), [sections, order]);

    const sensors = useSensors(useSensor(PointerSensor, { activationConstraint: { distance: 8 } }));

    const handleDragEnd = useCallback(
        ({ active, over }: DragEndEvent) => {
            if (!over || active.id === over.id) return;

            const activeKey = String(active.id);
            const overKey = String(over.id);
            const activeIndex = order.indexOf(activeKey);
            const overIndex = order.indexOf(overKey);
            if (activeIndex < 0 || overIndex < 0) return;

            const nextOrder = arrayMove(order, activeIndex, overIndex);
            setOrder(nextOrder);
            onOrderChange?.(nextOrder);
        },
        [onOrderChange, order]
    );

    if (orderedSections.length === 0) {
        return empty ?? null;
    }

    return (
        <DndContext
            sensors={sensors}
            collisionDetection={closestCenter}
            onDragEnd={disabled ? undefined : handleDragEnd}
        >
            <SortableContext
                items={orderedSections.map((section) => section.key)}
                strategy={verticalListSortingStrategy}
            >
                <Space
                    direction="vertical"
                    className={className ? className : 'w-full'}
                    size="large"
                >
                    {orderedSections.map(children)}
                </Space>
            </SortableContext>
        </DndContext>
    );
};

interface SortableGroupItemListProps<T extends { id: string }> {
    items: T[];
    children: (item: T, dragHandle: ReactNode) => ReactNode;
    empty?: ReactNode;
    className?: string;
    style?: CSSProperties;
    disabled?: boolean;
    onOrderChange?: (nextOrder: string[]) => void;
}

const reconcileItemOrder = (previousOrder: string[], currentIds: string[]) => {
    if (currentIds.length === 0) return [];
    if (previousOrder.length === 0) return currentIds;

    const currentIdSet = new Set(currentIds);
    const filtered = previousOrder.filter((id) => currentIdSet.has(id));
    const missing = currentIds.filter((id) => !filtered.includes(id));

    if (filtered.length === 0) return currentIds;
    const nextOrder = [...filtered, ...missing];
    return areOrdersEqual(previousOrder, nextOrder) ? previousOrder : nextOrder;
};

export const SortableGroupItemList = <T extends { id: string }>({
    items,
    children,
    empty,
    className,
    style,
    disabled = false,
    onOrderChange,
}: SortableGroupItemListProps<T>) => {
    const currentIds = useMemo(() => items.map((item) => item.id), [items]);
    const [order, setOrder] = useState<string[]>(currentIds);

    useEffect(() => {
        setOrder((previousOrder) => reconcileItemOrder(previousOrder, currentIds));
    }, [currentIds]);

    const orderedItems = useMemo(() => {
        const orderIndex = new Map<string, number>(order.map((id, index) => [id, index]));
        const originalIndex = new Map<string, number>(items.map((item, index) => [item.id, index]));

        return [...items].sort((left, right) => {
            const leftOrder = orderIndex.get(left.id);
            const rightOrder = orderIndex.get(right.id);

            if (leftOrder !== undefined && rightOrder !== undefined) {
                return leftOrder - rightOrder;
            }

            if (leftOrder !== undefined) return -1;
            if (rightOrder !== undefined) return 1;

            return (originalIndex.get(left.id) ?? 0) - (originalIndex.get(right.id) ?? 0);
        });
    }, [items, order]);

    const sensors = useSensors(useSensor(PointerSensor, { activationConstraint: { distance: 8 } }));

    const handleDragEnd = useCallback(
        ({ active, over }: DragEndEvent) => {
            if (!over || active.id === over.id) return;

            const activeId = String(active.id);
            const overId = String(over.id);
            const activeIndex = order.indexOf(activeId);
            const overIndex = order.indexOf(overId);
            if (activeIndex < 0 || overIndex < 0) return;

            const nextOrder = arrayMove(order, activeIndex, overIndex);
            setOrder(nextOrder);
            onOrderChange?.(nextOrder);
        },
        [onOrderChange, order]
    );

    if (orderedItems.length === 0) {
        return empty ?? null;
    }

    return (
        <DndContext
            sensors={sensors}
            collisionDetection={closestCenter}
            onDragEnd={disabled ? undefined : handleDragEnd}
        >
            <SortableContext
                items={orderedItems.map((item) => item.id)}
                strategy={rectSortingStrategy}
            >
                <div className={className} style={style}>
                    {orderedItems.map((item) => (
                        <SortableGroupItemFrame
                            key={item.id}
                            id={item.id}
                            disabled={disabled}
                            className="h-full"
                        >
                            {(dragHandle) => children(item, dragHandle)}
                        </SortableGroupItemFrame>
                    ))}
                </div>
            </SortableContext>
        </DndContext>
    );
};

const SortableGroupTableDisabledContext = createContext(false);
const SortableGroupTableDragHandleContext = createContext<ReactNode>(null);

const assignForwardedRef = <T,>(ref: ForwardedRef<T> | null | undefined, node: T | null) => {
    if (!ref) return;
    if (typeof ref === 'function') {
        ref(node);
        return;
    }
    (ref as { current: T | null }).current = node;
};

const mergeForwardedRefs =
    <T,>(...refs: Array<ForwardedRef<T> | undefined>) =>
    (node: T | null) => {
        refs.forEach((ref) => {
            assignForwardedRef(ref, node);
        });
    };

const SortableGroupTableDragHandleCell = memo(() => {
    const dragHandle = useContext(SortableGroupTableDragHandleContext);

    return <div className="flex items-center justify-center">{dragHandle}</div>;
});

interface SortableGroupTableRowProps extends HTMLAttributes<HTMLTableRowElement> {
    'data-row-key'?: string | number;
}

const SortableGroupTableRow = forwardRef<HTMLTableRowElement, SortableGroupTableRowProps>(
    ({ children, style, ...restProps }, ref) => {
        const disabled = useContext(SortableGroupTableDisabledContext);
        const rowKey = String(restProps['data-row-key'] ?? '');
        const { attributes, listeners, setNodeRef, transform, transition, isDragging } =
            useSortable({
                id: rowKey,
                disabled,
            });

        const dragHandle = (
            <Button
                type="text"
                size="small"
                icon={<HolderOutlined />}
                disabled={disabled}
                aria-label="拖拽排序"
                title="拖拽排序"
                className="!cursor-grab active:!cursor-grabbing"
                style={{ touchAction: 'none' }}
                {...attributes}
                {...listeners}
            />
        );

        return (
            <SortableGroupTableDragHandleContext.Provider value={dragHandle}>
                <tr
                    ref={mergeForwardedRefs(ref, setNodeRef)}
                    {...restProps}
                    style={{
                        ...style,
                        transform: CSS.Transform.toString(transform),
                        transition: transition || undefined,
                        opacity: isDragging ? 0.75 : 1,
                        zIndex: isDragging ? 1 : undefined,
                    }}
                >
                    {children}
                </tr>
            </SortableGroupTableDragHandleContext.Provider>
        );
    }
);

interface SortableGroupTableListProps<T extends { id: string }> {
    items: T[];
    columns: ColumnsType<T>;
    empty?: ReactNode;
    className?: string;
    style?: CSSProperties;
    disabled?: boolean;
    onOrderChange?: (nextOrder: string[]) => void;
    tableProps?: Omit<TableProps<T>, 'columns' | 'dataSource' | 'rowKey' | 'components'>;
}

export const SortableGroupTableList = <T extends { id: string }>({
    items,
    columns,
    empty,
    className,
    style,
    disabled = false,
    onOrderChange,
    tableProps,
}: SortableGroupTableListProps<T>) => {
    const currentIds = useMemo(() => items.map((item) => item.id), [items]);
    const [order, setOrder] = useState<string[]>(currentIds);

    useEffect(() => {
        setOrder((previousOrder) => reconcileItemOrder(previousOrder, currentIds));
    }, [currentIds]);

    const orderedItems = useMemo(() => {
        const orderIndex = new Map<string, number>(order.map((id, index) => [id, index]));
        const originalIndex = new Map<string, number>(items.map((item, index) => [item.id, index]));

        return [...items].sort((left, right) => {
            const leftOrder = orderIndex.get(left.id);
            const rightOrder = orderIndex.get(right.id);

            if (leftOrder !== undefined && rightOrder !== undefined) {
                return leftOrder - rightOrder;
            }

            if (leftOrder !== undefined) return -1;
            if (rightOrder !== undefined) return 1;

            return (originalIndex.get(left.id) ?? 0) - (originalIndex.get(right.id) ?? 0);
        });
    }, [items, order]);

    const sensors = useSensors(useSensor(PointerSensor, { activationConstraint: { distance: 8 } }));

    const handleDragEnd = useCallback(
        ({ active, over }: DragEndEvent) => {
            if (!over || active.id === over.id) return;

            const activeId = String(active.id);
            const overId = String(over.id);
            const activeIndex = order.indexOf(activeId);
            const overIndex = order.indexOf(overId);
            if (activeIndex < 0 || overIndex < 0) return;

            const nextOrder = arrayMove(order, activeIndex, overIndex);
            setOrder(nextOrder);
            onOrderChange?.(nextOrder);
        },
        [onOrderChange, order]
    );

    const columnsWithHandle = useMemo<ColumnsType<T>>(
        () => [
            {
                key: '__sortable_drag_handle__',
                title: '',
                width: 48,
                fixed: 'left' as const,
                align: 'center',
                render: () => <SortableGroupTableDragHandleCell />,
            },
            ...columns,
        ],
        [columns]
    );

    if (orderedItems.length === 0) {
        return empty ?? null;
    }

    const locale = {
        ...(tableProps?.locale ?? {}),
        emptyText: empty ?? tableProps?.locale?.emptyText ?? <Empty description="暂无数据" />,
    };

    return (
        <SortableGroupTableDisabledContext.Provider value={disabled}>
            <DndContext
                sensors={sensors}
                collisionDetection={closestCenter}
                onDragEnd={disabled ? undefined : handleDragEnd}
            >
                <SortableContext
                    items={orderedItems.map((item) => item.id)}
                    strategy={verticalListSortingStrategy}
                >
                    <div className={className ? className : 'w-full'} style={style}>
                        <Table
                            {...tableProps}
                            rowKey="id"
                            pagination={false}
                            dataSource={orderedItems}
                            columns={columnsWithHandle}
                            components={{ body: { row: SortableGroupTableRow } }}
                            locale={locale}
                        />
                    </div>
                </SortableContext>
            </DndContext>
        </SortableGroupTableDisabledContext.Provider>
    );
};
