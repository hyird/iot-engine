import {
    ApartmentOutlined,
    DeleteOutlined,
    DownOutlined,
    EditOutlined,
    PlusOutlined,
    ShareAltOutlined,
} from '@ant-design/icons';
import { App, Button, Dropdown, Popover, Space, Spin, Tree } from 'antd';
import type { DataNode, TreeProps } from 'antd/es/tree';
import { useMemo, useState } from 'react';
import DeviceGroupFormModal from './DeviceGroupFormModal';
import {
    useDeviceGroupDelete,
    useDeviceGroupSave,
    useDeviceGroupTreeWithCount,
} from './device.service';
import type { DeviceGroup } from './device-group.types';

interface DeviceGroupPanelProps {
    selectedGroupId: string | null;
    onSelect: (groupId: string | null) => void;
    canManageGroup: boolean;
    ungroupedCount: number;
    onShare: (group: DeviceGroup.TreeItem) => void;
}

type TreeKey = string | number;

const DeviceGroupPanel = ({
    selectedGroupId,
    onSelect,
    canManageGroup,
    ungroupedCount,
    onShare,
}: DeviceGroupPanelProps) => {
    const { modal } = App.useApp();
    const [popoverOpen, setPopoverOpen] = useState(false);
    const [formModalVisible, setFormModalVisible] = useState(false);
    const [editingGroup, setEditingGroup] = useState<DeviceGroup.TreeItem | null>(null);
    const [parentIdForCreate, setParentIdForCreate] = useState<string | null>(null);

    const { data: treeData = [], isLoading } = useDeviceGroupTreeWithCount();
    const saveMutation = useDeviceGroupSave();
    const deleteMutation = useDeviceGroupDelete();

    const groupIndex = useMemo(() => {
        const index = new Map<string, DeviceGroup.TreeItem>();
        const walk = (nodes: DeviceGroup.TreeItem[]) => {
            for (const node of nodes) {
                index.set(node.id, node);
                if (node.children?.length) {
                    walk(node.children);
                }
            }
        };
        walk(treeData);
        return index;
    }, [treeData]);

    const antTreeData = useMemo(() => {
        const convert = (nodes: DeviceGroup.TreeItem[]): DataNode[] =>
            nodes.map((node) => ({
                key: node.id,
                title: `${node.name} (${node.deviceCount ?? 0})`,
                children: node.children?.length ? convert(node.children) : undefined,
            }));

        return [
            { key: 'all', title: '全部设备', isLeaf: true } as DataNode,
            ...(ungroupedCount > 0
                ? [
                      {
                          key: 'ungrouped',
                          title: `未分组 (${ungroupedCount})`,
                          isLeaf: true,
                      } as DataNode,
                  ]
                : []),
            ...convert(treeData),
        ];
    }, [treeData, ungroupedCount]);

    const selectedKeys = useMemo<TreeKey[]>(() => {
        if (selectedGroupId === null) return ['all'];
        if (selectedGroupId === 'ungrouped') return ['ungrouped'];
        return [selectedGroupId];
    }, [selectedGroupId]);

    // 当前选中的分组名称（用于按钮显示）
    const selectedLabel = useMemo(() => {
        if (selectedGroupId === null) return '全部设备';
        if (selectedGroupId === 'ungrouped') return '未分组';
        return groupIndex.get(selectedGroupId)?.name ?? '全部设备';
    }, [groupIndex, selectedGroupId]);

    const handleSelect: TreeProps['onSelect'] = (keys) => {
        if (!keys.length) return;
        const key = keys[0];
        if (key === 'all') onSelect(null);
        else if (key === 'ungrouped') onSelect('ungrouped');
        else onSelect(String(key));
        setPopoverOpen(false);
    };

    const handleAddChild = (parentId: string) => {
        setEditingGroup(null);
        setParentIdForCreate(parentId);
        setFormModalVisible(true);
    };

    const handleEdit = (id: string) => {
        const group = groupIndex.get(id);
        if (group) {
            setEditingGroup(group);
            setParentIdForCreate(null);
            setFormModalVisible(true);
        }
    };

    const handleDelete = (id: string) => {
        const group = groupIndex.get(id);
        if (!group) return;
        modal.confirm({
            title: `确认删除分组「${group.name}」？`,
            content: '删除后该分组下的子分组和设备不会被删除，但需要先移除或转移。',
            okText: '确定删除',
            okButtonProps: { danger: true },
            onOk: () => deleteMutation.mutate(id),
        });
    };

    const contextMenuItems = (nodeKey: TreeKey) => {
        if (nodeKey === 'all' || nodeKey === 'ungrouped') return [];
        const items = [];
        const group = groupIndex.get(String(nodeKey));
        if (group?.can_share) {
            items.push({ key: 'share', label: '分享分组', icon: <ShareAltOutlined /> });
        }
        if (canManageGroup) {
            items.push(
                { key: 'addChild', label: '新增子分组', icon: <PlusOutlined /> },
                { key: 'edit', label: '编辑', icon: <EditOutlined /> },
                { key: 'delete', label: '删除', icon: <DeleteOutlined />, danger: true }
            );
        }
        return items;
    };

    const treeContent = (
        <div className="w-72 max-w-[calc(100vw-32px)]">
            {isLoading ? (
                <div className="py-6 text-center">
                    <Spin size="small" />
                </div>
            ) : (
                <div className="max-h-[min(68vh,560px)] overflow-y-auto pr-1">
                    <Tree
                        treeData={antTreeData}
                        selectedKeys={selectedKeys}
                        onSelect={handleSelect}
                        defaultExpandAll
                        blockNode
                        titleRender={(node) => {
                            const items = contextMenuItems(node.key as TreeKey);
                            const title = (
                                <span className="block whitespace-normal break-words pr-2">
                                    {node.title as string}
                                </span>
                            );
                            if (!items.length) return title;
                            return (
                                <Dropdown
                                    menu={{
                                        items,
                                        onClick: ({ key }) => {
                                            const id = String(node.key);
                                            if (key === 'share') {
                                                const group = groupIndex.get(id);
                                                if (group) onShare(group);
                                            } else if (key === 'addChild') handleAddChild(id);
                                            else if (key === 'edit') handleEdit(id);
                                            else if (key === 'delete') handleDelete(id);
                                        },
                                    }}
                                    trigger={['contextMenu']}
                                >
                                    {title}
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
                    onClick={() => {
                        setEditingGroup(null);
                        setParentIdForCreate(null);
                        setFormModalVisible(true);
                    }}
                >
                    新建分组
                </Button>
            )}
        </div>
    );

    return (
        <>
            <Popover
                content={treeContent}
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

            <DeviceGroupFormModal
                open={formModalVisible}
                editing={editingGroup}
                parentId={parentIdForCreate}
                treeData={treeData}
                loading={saveMutation.isPending}
                onCancel={() => {
                    setFormModalVisible(false);
                    setEditingGroup(null);
                }}
                onFinish={(values) =>
                    saveMutation.mutate(values, {
                        onSuccess: () => {
                            setFormModalVisible(false);
                            setEditingGroup(null);
                        },
                    })
                }
            />
        </>
    );
};

export default DeviceGroupPanel;
