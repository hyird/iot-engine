import {
    ApartmentOutlined,
    DeleteOutlined,
    DownOutlined,
    EditOutlined,
    PlusOutlined,
} from '@ant-design/icons';
import { App, Button, Dropdown, Popover, Space, Spin, Tree } from 'antd';
import type { DataNode, TreeProps } from 'antd/es/tree';
import { useMemo, useState } from 'react';
import EdgeNodeGroupFormModal from './EdgeNodeGroupFormModal';
import { useEdgeGroupDelete, useEdgeGroupSave, useEdgeGroupTree } from './edge-node.service';
import type { Edge } from './edge-node.types';

interface Props {
    selectedGroupId: string | null;
    onSelect: (groupId: string | null) => void;
    canManageGroup: boolean;
    ungroupedCount: number;
}

type TreeKey = string | number;

export default function EdgeNodeGroupPanel({
    selectedGroupId,
    onSelect,
    canManageGroup,
    ungroupedCount,
}: Props) {
    const { modal } = App.useApp();
    const [popoverOpen, setPopoverOpen] = useState(false);
    const [formOpen, setFormOpen] = useState(false);
    const [editingGroup, setEditingGroup] = useState<Edge.GroupTreeItem | null>(null);
    const [parentId, setParentId] = useState<string | null>(null);
    const { data: groups = [], isLoading } = useEdgeGroupTree();
    const save = useEdgeGroupSave();
    const remove = useEdgeGroupDelete();

    const groupIndex = useMemo(() => {
        const index = new Map<string, Edge.GroupTreeItem>();
        const walk = (items: Edge.GroupTreeItem[]) => {
            for (const item of items) {
                index.set(item.id, item);
                if (item.children?.length) walk(item.children);
            }
        };
        walk(groups);
        return index;
    }, [groups]);

    const treeData = useMemo<DataNode[]>(() => {
        const convert = (items: Edge.GroupTreeItem[]): DataNode[] =>
            items.map((item) => ({
                key: item.id,
                title: `${item.name} (${item.nodeCount})${item.status === 'disabled' ? ' · 已停用' : ''}`,
                children: item.children?.length ? convert(item.children) : undefined,
            }));
        return [
            { key: 'all', title: '全部节点', isLeaf: true },
            ...(ungroupedCount > 0
                ? [{ key: 'ungrouped', title: `未分组 (${ungroupedCount})`, isLeaf: true }]
                : []),
            ...convert(groups),
        ];
    }, [groups, ungroupedCount]);

    const selectedLabel = useMemo(() => {
        if (selectedGroupId === null) return '全部节点';
        if (selectedGroupId === 'ungrouped') return '未分组';
        return groupIndex.get(selectedGroupId)?.name ?? '全部节点';
    }, [groupIndex, selectedGroupId]);

    const selectedKeys: TreeKey[] = [selectedGroupId ?? 'all'];
    const onTreeSelect: TreeProps['onSelect'] = (keys) => {
        if (!keys.length) return;
        const key = String(keys[0]);
        onSelect(key === 'all' ? null : key);
        setPopoverOpen(false);
    };

    const openCreate = (nextParentId: string | null) => {
        setEditingGroup(null);
        setParentId(nextParentId);
        setFormOpen(true);
    };
    const openEdit = (id: string) => {
        const group = groupIndex.get(id);
        if (!group) return;
        setEditingGroup(group);
        setParentId(null);
        setFormOpen(true);
    };
    const confirmDelete = (id: string) => {
        const group = groupIndex.get(id);
        if (!group) return;
        modal.confirm({
            title: `确认删除分组「${group.name}」？`,
            content: '请先移出该分组的子分组和边缘节点。',
            okText: '确定删除',
            okButtonProps: { danger: true },
            onOk: () => remove.mutateAsync(id),
        });
    };

    const content = (
        <div className="w-72 max-w-[calc(100vw-32px)]">
            {isLoading ? (
                <div className="py-6 text-center">
                    <Spin size="small" />
                </div>
            ) : (
                <div className="max-h-[min(68vh,560px)] overflow-y-auto pr-1">
                    <Tree
                        blockNode
                        defaultExpandAll
                        treeData={treeData}
                        selectedKeys={selectedKeys}
                        onSelect={onTreeSelect}
                        titleRender={(node) => {
                            const key = String(node.key);
                            if (!canManageGroup || key === 'all' || key === 'ungrouped')
                                return <span>{node.title as string}</span>;
                            return (
                                <Dropdown
                                    trigger={['contextMenu']}
                                    menu={{
                                        items: [
                                            {
                                                key: 'add',
                                                label: '新增子分组',
                                                icon: <PlusOutlined />,
                                            },
                                            {
                                                key: 'edit',
                                                label: '编辑',
                                                icon: <EditOutlined />,
                                            },
                                            {
                                                key: 'delete',
                                                label: '删除',
                                                icon: <DeleteOutlined />,
                                                danger: true,
                                            },
                                        ],
                                        onClick: ({ key: action }) => {
                                            if (action === 'add') openCreate(key);
                                            else if (action === 'edit') openEdit(key);
                                            else confirmDelete(key);
                                        },
                                    }}
                                >
                                    <span className="block whitespace-normal break-words pr-2">
                                        {node.title as string}
                                    </span>
                                </Dropdown>
                            );
                        }}
                    />
                </div>
            )}
            {canManageGroup && (
                <Button
                    type="text"
                    size="small"
                    block
                    icon={<PlusOutlined />}
                    className="mt-1 !text-gray-500"
                    onClick={() => openCreate(null)}
                >
                    新建分组
                </Button>
            )}
        </div>
    );

    return (
        <>
            <Popover
                content={content}
                trigger="click"
                open={popoverOpen}
                onOpenChange={setPopoverOpen}
                placement="bottomLeft"
            >
                <Button icon={<ApartmentOutlined />}>
                    <Space size={4}>
                        {selectedLabel}
                        <DownOutlined className="!text-[10px] text-gray-400" />
                    </Space>
                </Button>
            </Popover>
            <EdgeNodeGroupFormModal
                open={formOpen}
                editing={editingGroup}
                parentId={parentId}
                treeData={groups}
                loading={save.isPending}
                onCancel={() => {
                    setFormOpen(false);
                    setEditingGroup(null);
                }}
                onFinish={(values) =>
                    save.mutate(values, {
                        onSuccess: () => {
                            setFormOpen(false);
                            setEditingGroup(null);
                        },
                    })
                }
            />
        </>
    );
}
