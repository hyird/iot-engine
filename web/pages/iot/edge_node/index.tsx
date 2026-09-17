import { LiveQueryError } from '@/components/LiveQueryError';
import {
    ApartmentOutlined,
    CheckOutlined,
    CloudServerOutlined,
    CodeOutlined,
    DeleteOutlined,
    DownloadOutlined,
    DownOutlined,
    EditOutlined,
    EyeOutlined,
    GlobalOutlined,
    PlusOutlined,
    ReloadOutlined,
    SyncOutlined,
    UploadOutlined,
} from '@ant-design/icons';
import { FitAddon } from '@xterm/addon-fit';
import { WebglAddon } from '@xterm/addon-webgl';
import { Terminal } from '@xterm/xterm';
import {
    Alert,
    App,
    Button,
    Descriptions,
    Drawer,
    Dropdown,
    Empty,
    Flex,
    Form,
    Input,
    InputNumber,
    Modal,
    Popconfirm,
    Popover,
    Progress,
    Result,
    Select,
    Skeleton,
    Space,
    Spin,
    Switch,
    Table,
    Tabs,
    Tag,
    Tooltip,
    Tree,
    TreeSelect,
    Upload,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import type { DataNode, TreeProps } from 'antd/es/tree';
import { useEffect, useMemo, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { usePermissions } from '@/hooks/usePermission';
import {
    getWindowsClientDownloadUrl,
    useEdgeGroupDelete,
    useEdgeGroupSave,
    useEdgeGroupTree,
    useEdgeVpn,
    useEdgeVpnPeerCreate,
    useEdgeVpnPeerRevoke,
    useEdgeVpnPeerSync,
    useEdgeVpnRouteUpdate,
} from './edge_node.service';
import type { Edge, EdgeVpn } from './edge_node.types';
import '@xterm/xterm/css/xterm.css';
import type { CSSProperties, ReactNode } from 'react';
import { useRef } from 'react';
import type { DeviceCardItem } from '@/components/DeviceCard';
import DeviceCard from '@/components/DeviceCard';
import { PageContainer } from '@/components/PageContainer';
import { formatDateTime } from '@/utils/dateTime';
import { validateForm } from '@/utils/validation';
import {
    firmwareUpgradeSchema,
    networkInterfaceSchema,
    networkSchema,
    nodeNameSchema,
    serialSettingsSchema,
    terminalEventsSchema,
} from './edge_node.schema';
import {
    acknowledgeTerminalOutput,
    buildEdgeNodeGroupView,
    closeTerminal,
    getEdgeDetail,
    getTerminalEvents,
    keepTerminalAlive,
    normalizeReportedNetwork,
    openTerminal,
    physicalNetworkInterfaces,
    resizeTerminal,
    serialPayloadHex,
    useAssignEdgeNodeGroup,
    useConfigureEdgeNetwork,
    useDeviceConfigSyncMutation,
    useEdgeDeleteMutation,
    useEdgeDetail,
    useEdgeInventory,
    useEdgeLogs,
    useEnrollmentMutation,
    useFirmwareUpgradeMutation,
    useRenameEdgeNode,
    useSerialDebug,
    useSetEdgeLogLevel,
    writeTerminal,
} from './edge_node.service';

interface Props {
    open: boolean;
    editing: Edge.GroupTreeItem | null;
    parentId: string | null;
    treeData: Edge.GroupTreeItem[];
    loading: boolean;
    onCancel: () => void;
    onFinish: (
        values: Edge.GroupSaveDto & {
            id?: string;
        }
    ) => void;
}
function groupOptions(
    nodes: Edge.GroupTreeItem[],
    excludeId?: string
): {
    value: string;
    title: string;
    children?: ReturnType<typeof groupOptions>;
}[] {
    return nodes
        .filter((node) => node.id !== excludeId)
        .map((node) => ({
            value: node.id,
            title: node.name,
            children: node.children?.length ? groupOptions(node.children, excludeId) : undefined,
        }));
}
export function EdgeNodeGroupFormModal({
    open,
    editing,
    parentId,
    treeData,
    loading,
    onCancel,
    onFinish,
}: Props) {
    const [form] = Form.useForm<
        Edge.GroupSaveDto & {
            id?: string;
        }
    >();
    useEffect(() => {
        if (!open) return;
        if (editing) {
            form.setFieldsValue({
                id: editing.id,
                name: editing.name,
                parentId: editing.parentId || undefined,
                sortOrder: editing.sortOrder,
                status: editing.status,
                remark: editing.remark,
            });
            return;
        }
        form.resetFields();
        form.setFieldsValue({
            parentId: parentId || undefined,
            sortOrder: 0,
            status: 'enabled',
            remark: '',
        });
    }, [editing, form, open, parentId]);
    const treeDataForSelect = useMemo(
        () => groupOptions(treeData, editing?.id),
        [editing?.id, treeData]
    );
    return (
        <FormModal
            open={open}
            title={editing ? '编辑边缘节点分组' : '新建边缘节点分组'}
            okText="确定"
            cancelText="取消"
            confirmLoading={loading}
            onCancel={onCancel}
            onOk={() => form.submit()}
            destroyOnClose
        >
            <Form form={form} layout="vertical" onFinish={onFinish} className="mt-4">
                <Form.Item name="id" hidden>
                    <Input />
                </Form.Item>
                <Form.Item
                    label="分组名称"
                    name="name"
                    rules={[{ required: true, message: '请输入分组名称' }]}
                >
                    <Input placeholder="请输入分组名称" maxLength={100} />
                </Form.Item>
                <Form.Item label="上级分组" name="parentId">
                    <TreeSelect
                        allowClear
                        treeData={treeDataForSelect}
                        placeholder="不选则为顶级分组"
                        treeDefaultExpandAll
                    />
                </Form.Item>
                <Form.Item label="排序" name="sortOrder">
                    <InputNumber className="!w-full" min={0} placeholder="数值越小越靠前" />
                </Form.Item>
                <Form.Item
                    label="状态"
                    name="status"
                    rules={[{ required: true, message: '请选择状态' }]}
                >
                    <Select
                        options={[
                            { value: 'enabled', label: '启用' },
                            { value: 'disabled', label: '禁用' },
                        ]}
                    />
                </Form.Item>
                <Form.Item label="备注" name="remark">
                    <Input.TextArea rows={2} maxLength={500} placeholder="可选备注" />
                </Form.Item>
            </Form>
        </FormModal>
    );
}

interface EdgeNodeGroupPanelProps {
    selectedGroupId: string | null;
    onSelect: (groupId: string | null) => void;
    canManageGroup: boolean;
    ungroupedCount: number;
    nodes: Edge.Node[];
}
type TreeKey = string | number;
export function EdgeNodeGroupPanel({
    selectedGroupId,
    onSelect,
    canManageGroup,
    ungroupedCount,
    nodes,
}: EdgeNodeGroupPanelProps) {
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
        const counts = new Map<string, number>();
        for (const node of nodes) counts.set(node.groupId, (counts.get(node.groupId) ?? 0) + 1);
        const convert = (items: Edge.GroupTreeItem[]): DataNode[] =>
            items.map((item) => ({
                key: item.id,
                title: `${item.name} (${counts.get(item.id) ?? 0})${item.status === 'disabled' ? ' · 已停用' : ''}`,
                children: item.children?.length ? convert(item.children) : undefined,
            }));
        return [
            { key: 'all', title: '全部节点', isLeaf: true },
            ...(ungroupedCount > 0
                ? [{ key: 'ungrouped', title: `未分组 (${ungroupedCount})`, isLeaf: true }]
                : []),
            ...convert(groups),
        ];
    }, [groups, ungroupedCount, nodes]);
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

const statusLabels: Record<
    string,
    {
        color: string;
        text: string;
    }
> = {
    pending: { color: 'processing', text: '等待节点上报公钥' },
    active: { color: 'success', text: '已启用' },
    revoked: { color: 'error', text: '已撤销' },
    error: { color: 'error', text: '错误' },
    disabled: { color: 'default', text: '已停用' },
};
function vpnStatusTag(status: string) {
    const item = statusLabels[status] ?? { color: 'default', text: status || '-' };
    return <Tag color={item.color}>{item.text}</Tag>;
}
function peerName(node: Edge.Node) {
    return `${node.name || node.hostname || node.imei} VPN`;
}
type RouteFormValues = Pick<EdgeVpn.RouteDto, 'virtualCidr'>;
const VPN_MODAL_Z_INDEX = 1100;
export function EdgeVpnPanel({ node, scope }: { node: Edge.Node; scope: Edge.EventScope }) {
    const { has } = usePermissions();
    const canQuery = has('iot:vpn:query');
    const canAdd = has('iot:vpn:add');
    const canEdit = has('iot:vpn:edit');
    const canRevoke = has('iot:vpn:revoke');
    const dataQuery = useEdgeVpn(scope, canQuery && Boolean(scope.vpn));
    const data = dataQuery.data;
    const peer = data?.peers.find((item) => item.peerType === 'edge' && item.status !== 'revoked');
    const network = data?.networks[0];
    const [routeOpen, setRouteOpen] = useState(false);
    const [editingRoute, setEditingRoute] = useState<EdgeVpn.Route | undefined>();
    const [routeForm] = Form.useForm<RouteFormValues>();
    const peerCreate = useEdgeVpnPeerCreate();
    const peerSync = useEdgeVpnPeerSync();
    const peerRevoke = useEdgeVpnPeerRevoke();
    const routeUpdate = useEdgeVpnRouteUpdate();
    const bridgeNetworks = node.networks?.filter(
        (item) => item.bridge && item.ipv4 && item.prefixLength >= 1 && item.prefixLength <= 30
    );
    useEffect(() => {
        if (!routeOpen) return;
        if (editingRoute) routeForm.setFieldsValue({ virtualCidr: editingRoute.virtualCidr });
    }, [editingRoute, routeForm, routeOpen]);
    const routeColumns = useMemo<ColumnsType<EdgeVpn.Route>>(
        () => [
            { title: 'LAN 接口', dataIndex: 'lanInterface', width: 120 },
            { title: '真实网段', dataIndex: 'targetCidr', width: 140 },
            { title: '虚拟网段', dataIndex: 'virtualCidr', width: 140 },
            {
                title: '模式',
                dataIndex: 'mode',
                width: 90,
                render: (value: EdgeVpn.RouteMode) => (value === 'nat' ? 'NAT' : '路由'),
            },
            {
                title: '状态',
                dataIndex: 'status',
                width: 150,
                render: (value: string, item) =>
                    item.lastError ? (
                        <span title={item.lastError}>{vpnStatusTag(value)}</span>
                    ) : (
                        vpnStatusTag(value)
                    ),
            },
            {
                title: '操作',
                key: 'actions',
                width: 130,
                render: (_, item) => (
                    <Space size={0}>
                        {canEdit && (
                            <Button
                                type="link"
                                size="small"
                                onClick={() => {
                                    setEditingRoute(item);
                                    setRouteOpen(true);
                                }}
                            >
                                编辑
                            </Button>
                        )}
                    </Space>
                ),
            },
        ],
        [canEdit]
    );
    if (!canQuery) {
        return <Alert type="warning" showIcon message="您没有 VPN 查询权限" />;
    }
    const supportsVpn = node.capability.vpn?.supportsVpn === true;
    const hasBridgeNetwork = Boolean(bridgeNetworks?.length);
    const closeRoute = () => {
        if (!routeUpdate.isPending) {
            setRouteOpen(false);
            setEditingRoute(undefined);
        }
    };
    return (
        <div className="space-y-4">
            {!supportsVpn && (
                <Alert
                    type="warning"
                    showIcon
                    message="当前节点未上报 WireGuard 能力"
                    description="请将节点代理升级到支持 VPN 的版本；旧节点仍可正常使用其他功能。"
                />
            )}
            {supportsVpn && !hasBridgeNetwork && (
                <Alert
                    type="warning"
                    showIcon
                    message="当前节点没有可映射的桥接 LAN"
                    description="请先让 EdgeNode 上报桥接网段，VPN 会根据该网段自动生成等长的虚拟映射。"
                />
            )}
            <LiveQueryError
                error={dataQuery.error}
                retry={dataQuery.refetch}
                loading={dataQuery.isFetching}
            />
            <Flex justify="space-between" align="center" gap={12} wrap>
                <div>
                    <div className="font-medium text-slate-800">节点 VPN · iot-server</div>
                    <div className="mt-1 text-xs text-slate-500">
                        使用 iot-server 的 WireGuard Server。真实 LAN、桥接接口和掩码由节点上报；
                        这里只允许修改虚拟网段网络号。
                    </div>
                </div>
                <Space wrap>
                    <Button
                        icon={<ReloadOutlined />}
                        loading={dataQuery.isFetching}
                        onClick={() => void dataQuery.refetch()}
                    >
                        刷新
                    </Button>
                    {!peer && canAdd && (
                        <Button
                            type="primary"
                            icon={<CloudServerOutlined />}
                            loading={peerCreate.isPending}
                            disabled={!supportsVpn || !data?.networks.length || !hasBridgeNetwork}
                            onClick={() =>
                                peerCreate.mutate({
                                    peerType: 'edge',
                                    edgeNodeId: node.id,
                                    name: peerName(node),
                                })
                            }
                        >
                            启用节点 VPN
                        </Button>
                    )}
                </Space>
            </Flex>

            {dataQuery.isLoading ? (
                <Skeleton active paragraph={{ rows: 4 }} />
            ) : !data?.networks.length ? (
                <Empty description="默认 iot-server VPN 网络尚未就绪" />
            ) : (
                <>
                    <Descriptions bordered size="small" column={{ xs: 1, sm: 2, lg: 4 }}>
                        <Descriptions.Item label="VPN 网络">
                            {network?.name ?? '-'}
                        </Descriptions.Item>
                        <Descriptions.Item label="Overlay">
                            {network?.overlayCidr ?? '-'}
                        </Descriptions.Item>
                        <Descriptions.Item label="节点地址">
                            {peer?.assignedIpv4 ?? '-'}
                        </Descriptions.Item>
                        <Descriptions.Item label="Peer 状态">
                            {peer ? vpnStatusTag(peer.status) : <Tag>未加入</Tag>}
                        </Descriptions.Item>
                        <Descriptions.Item label="Agent / WireGuard" span={2}>
                            {node.capability.vpn?.agentVersion || '-'} /{' '}
                            {node.capability.vpn?.wireguardVersion || '-'}
                        </Descriptions.Item>
                        <Descriptions.Item label="配置版本">
                            {peer?.configRevision ?? '-'}
                        </Descriptions.Item>
                        <Descriptions.Item label="公钥">
                            {node.capability.vpn?.publicKey || peer?.publicKey || '-'}
                        </Descriptions.Item>
                    </Descriptions>

                    {peer && (
                        <Flex justify="end" gap={8} wrap>
                            {canEdit && (
                                <Button
                                    icon={<SyncOutlined />}
                                    loading={peerSync.isPending}
                                    onClick={() => peerSync.mutate(peer.id)}
                                >
                                    重新下发配置
                                </Button>
                            )}
                            {canRevoke && peer.status !== 'revoked' && (
                                <Popconfirm
                                    title="确认撤销当前节点的 VPN Peer 吗？"
                                    description="撤销后节点将不能继续访问 VPN 网络。"
                                    onConfirm={() => peerRevoke.mutate(peer.id)}
                                >
                                    <Button danger loading={peerRevoke.isPending}>
                                        撤销 Peer
                                    </Button>
                                </Popconfirm>
                            )}
                        </Flex>
                    )}

                    <Flex justify="space-between" align="center" gap={12}>
                        <span className="text-sm font-medium text-slate-800">LAN 路由映射</span>
                    </Flex>
                    <Table
                        rowKey="id"
                        size="small"
                        pagination={false}
                        columns={routeColumns}
                        dataSource={peer ? data.routes : []}
                        locale={{ emptyText: peer ? '暂无路由映射' : '请先将节点加入 VPN 网络' }}
                        scroll={{ x: 'max-content' }}
                    />
                </>
            )}

            <FormModal
                open={routeOpen}
                title="修改虚拟网段"
                onCancel={closeRoute}
                onOk={() => routeForm.submit()}
                confirmLoading={routeUpdate.isPending}
                destroyOnHidden
                zIndex={VPN_MODAL_Z_INDEX}
            >
                <Form
                    form={routeForm}
                    layout="vertical"
                    onFinish={(values) => {
                        if (!editingRoute) return;
                        routeUpdate.mutate(
                            { id: editingRoute.id, data: values },
                            { onSuccess: closeRoute }
                        );
                    }}
                >
                    <Descriptions bordered size="small" column={1} className="mb-4">
                        <Descriptions.Item label="桥接接口">
                            {editingRoute?.lanInterface ?? '-'}
                        </Descriptions.Item>
                        <Descriptions.Item label="真实 LAN">
                            {editingRoute?.targetCidr ?? '-'}
                        </Descriptions.Item>
                        <Descriptions.Item label="模式">NAT（自动）</Descriptions.Item>
                    </Descriptions>
                    <p className="mb-4 text-xs text-slate-500">
                        真实 LAN、桥接接口、掩码和 NAT 模式由 EdgeNode
                        自动确定；只允许修改虚拟网段网络号， 且必须与真实 LAN
                        使用相同掩码并保持全局唯一。
                    </p>
                    <Form.Item
                        name="virtualCidr"
                        label="虚拟 LAN 网段"
                        rules={[
                            { required: true, message: '请输入虚拟 LAN 网段' },
                            {
                                pattern: /^172\.\d{1,3}\.\d{1,3}\.\d{1,3}\/\d{1,2}$/,
                                message: '请输入 172.0.0.0/8 范围内的网络 CIDR',
                            },
                        ]}
                    >
                        <Input placeholder="172.168.1.0/24" />
                    </Form.Item>
                </Form>
            </FormModal>
        </div>
    );
}

type NetworkDraftItem = Edge.NetworkConfig & {
    sourceName?: string;
    original: boolean;
    dirty: boolean;
    up?: boolean;
};
const EDGE_CARD_GRID_CLASS = 'grid grid-cols-1 gap-3 xl:grid-cols-2 2xl:grid-cols-4';
const EDGE_DETAIL_DRAWER_Z_INDEX = 1000;
const EDGE_ACTION_MODAL_Z_INDEX = EDGE_DETAIL_DRAWER_Z_INDEX + 100;
function groupSelectOptions(groups: Edge.GroupTreeItem[]): {
    value: string;
    title: string;
    disabled: boolean;
    children?: ReturnType<typeof groupSelectOptions>;
}[] {
    return groups.map((group) => ({
        value: group.id,
        title: group.name,
        disabled: group.status === 'disabled',
        children: group.children?.length ? groupSelectOptions(group.children) : undefined,
    }));
}
function statusTag(status: string) {
    const map: Record<
        string,
        {
            color: string;
            text: string;
        }
    > = {
        pending: { color: 'processing', text: '待处理' },
        approved: { color: 'success', text: '已批准' },
        rejected: { color: 'error', text: '已拒绝' },
        applied: { color: 'success', text: '已应用' },
        succeeded: { color: 'success', text: '成功' },
        accepted: { color: 'processing', text: '已接收' },
        running: { color: 'processing', text: '执行中' },
        failed: { color: 'error', text: '失败' },
        idle: { color: 'default', text: '未下发' },
    };
    const item = map[status] ?? { color: 'default', text: status || '-' };
    return <Tag color={item.color}>{item.text}</Tag>;
}
function logLevelTag(level: string) {
    const map: Record<string, string> = {
        debug: 'default',
        info: 'processing',
        warn: 'warning',
        error: 'error',
    };
    return <Tag color={map[level] ?? 'default'}>{level || '-'}</Tag>;
}
function networkDraftFromReported(
    item: Edge.Network,
    interfaces: Edge.NetworkInterface[]
): NetworkDraftItem {
    const binding = normalizeReportedNetwork(item, interfaces);
    return {
        operation: 'upsert',
        name: item.name,
        sourceName: item.name,
        mode: item.mode === 'static' ? 'static' : 'dhcp',
        device: binding.device,
        bridge: binding.bridge,
        bridgePorts: binding.bridgePorts,
        ip: item.mode === 'static' ? item.ipv4 : '',
        prefixLength: item.mode === 'static' ? item.prefixLength : 0,
        gateway: item.mode === 'static' ? item.gateway : '',
        original: true,
        dirty: false,
        up: item.up,
    };
}
function formatBytes(value: number) {
    if (value < 1024) return `${value} B`;
    if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
    return `${(value / 1024 / 1024).toFixed(1)} MiB`;
}
function validConfigTimestamp(value: number) {
    return value >= Date.UTC(2020, 0, 1);
}
function formatConfigVersions(active: number, desired: number) {
    const activeValid = validConfigTimestamp(active);
    const desiredValid = validConfigTimestamp(desired);
    if (activeValid && desiredValid && active === desired) return String(active);
    if (activeValid && desiredValid) {
        return `已应用 ${active} · 目标 ${desired}`;
    }
    if (desiredValid) return `${desired}（等待节点应用）`;
    if (activeValid) return `已应用 ${active}`;
    return '--';
}
function simStateText(state: Edge.Mobile['simState']) {
    const values: Record<Edge.Mobile['simState'], string> = {
        unknown: '未知',
        ready: '就绪',
        not_inserted: '未插卡',
        pin_required: '需要 PIN',
        puk_required: '需要 PUK',
        blocked: '已锁定',
    };
    return values[state] ?? '未知';
}
function mobileState(node: Edge.Node) {
    const mobile = node.mobile;
    if (!mobile.available) return '未检测到';
    if (mobile.simState !== 'ready') return `SIM ${simStateText(mobile.simState)}`;
    if (mobile.connected) return `已连接${mobile.ipv4 ? ` · ${mobile.ipv4}` : ''}`;
    return mobile.registered ? '已注册，未拨号' : '未注册';
}
function buildNodeCardItems(node: Edge.Node): DeviceCardItem[] {
    const status = node.status;
    const config = status.config;
    const outbox = status.outbox;
    const capability = node.capability;
    const mobile = node.mobile;
    const firmware = node.firmware;
    return [
        {
            key: 'vpnVirtualCidrs',
            label: 'VPN 虚拟网段',
            children: node.vpnVirtualCidrs?.length ? node.vpnVirtualCidrs.join('、') : '-',
        },
        { key: 'hostname', label: '主机名', children: node.hostname || '-' },
        { key: 'architecture', label: '系统架构', children: node.architecture || '-' },
        { key: 'openwrt', label: 'OpenWrt', children: node.openwrtRelease || '-' },
        {
            key: 'enrollment',
            label: '注册状态',
            children: <span className="[&_.ant-tag]:!m-0">{statusTag(node.enrollmentStatus)}</span>,
        },
        {
            key: 'configVersion',
            label: '配置版本',
            children: formatConfigVersions(config.activeVersion, config.desiredVersion),
        },
        {
            key: 'outbox',
            label: '待传缓存',
            children: `${outbox.records ?? 0} 条 / ${formatBytes(outbox.bytes ?? 0)}`,
        },
        {
            key: 'networkManager',
            label: '网络管理',
            children:
                capability.networkConfig && capability.networkConfigVersion >= 2
                    ? '可用'
                    : '需升级代理',
        },
        { key: 'mobileState', label: '4G 状态', children: mobileState(node) },
        { key: 'iccid', label: 'ICCID', children: mobile.iccid || '-' },
        {
            key: 'mobileSignal',
            label: '4G 信号',
            children: mobile.available
                ? `${mobile.signal.percent}%${mobile.signal.rssiDbm !== -1 ? ` · ${mobile.signal.rssiDbm} dBm` : ''}`
                : '-',
        },
        {
            key: 'mobileNetwork',
            label: 'APN / 运营商',
            children: [mobile.apn, mobile.operator].filter(Boolean).join(' / ') || '-',
        },
        ...(firmware.state === 'accepted' || firmware.state === 'running'
            ? [
                  {
                      key: 'firmwareProgress',
                      label: '固件下载',
                      children: (
                          <Progress
                              percent={firmware.progressPercent}
                              size="small"
                              status="active"
                              format={(percent) =>
                                  `${percent ?? 0}% · ${formatBytes(firmware.downloadedBytes)} / ${formatBytes(firmware.totalBytes)}`
                              }
                          />
                      ),
                      span: 2,
                  } satisfies DeviceCardItem,
              ]
            : []),
    ];
}
export function SerialDebugModal({
    nodeId,
    path,
    title,
    onClose,
}: {
    nodeId: string;
    path: string;
    title: string;
    onClose: () => void;
}) {
    const debug = useSerialDebug(nodeId, path);
    const { message } = App.useApp();
    const [form] = Form.useForm<Edge.SerialSettings>();
    const values = Form.useWatch([], form) as Edge.SerialSettings | undefined;
    const [display, setDisplay] = useState<'hex' | 'text'>('hex');
    const [sendMode, setSendMode] = useState<'hex' | 'text'>('hex');
    const [input, setInput] = useState('');
    const [ending, setEnding] = useState('');
    const [follow, setFollow] = useState(true);
    const logRef = useRef<HTMLDivElement>(null);
    useEffect(() => {
        form.setFieldsValue(debug.settings);
    }, [form, debug.settings]);
    useEffect(() => {
        if (follow && debug.frames.length && logRef.current)
            logRef.current.scrollTop = logRef.current.scrollHeight;
    }, [debug.frames, follow]);
    const ready = debug.connection === 'ready';
    const changed =
        values &&
        Object.keys(debug.settings).some(
            (key) =>
                values[key as keyof Edge.SerialSettings] !==
                debug.settings[key as keyof Edge.SerialSettings]
        );
    const run = (operation: () => void) => {
        try {
            operation();
        } catch (error) {
            void message.error(error instanceof Error ? error.message : '操作失败');
        }
    };
    return (
        <FormModal
            open
            title={`串口调试 · ${title} · ${path}`}
            zIndex={EDGE_ACTION_MODAL_Z_INDEX}
            onCancel={onClose}
            styles={{
                body: { display: 'flex', flexDirection: 'column', gap: 12, overflow: 'hidden' },
            }}
            footer={
                <div className="flex flex-col gap-2 text-left">
                    <Space wrap>
                        <Select
                            aria-label="发送格式"
                            value={sendMode}
                            onChange={setSendMode}
                            options={[
                                { value: 'hex', label: 'HEX 发送' },
                                { value: 'text', label: '文本发送（UTF-8）' },
                            ]}
                        />
                        {sendMode === 'text' && (
                            <Select
                                aria-label="发送换行"
                                value={ending}
                                onChange={setEnding}
                                options={[
                                    { value: '', label: '无换行' },
                                    { value: '\r', label: 'CR' },
                                    { value: '\n', label: 'LF' },
                                    { value: '\r\n', label: 'CRLF' },
                                ]}
                            />
                        )}
                        <span className="text-xs text-slate-500">单次最多 1024 字节</span>
                    </Space>
                    <Input.TextArea
                        aria-label="发送内容"
                        value={input}
                        onChange={(event) => setInput(event.target.value)}
                        autoSize={{ minRows: 2, maxRows: 3 }}
                        maxLength={8192}
                        placeholder={
                            sendMode === 'hex' ? '01 03 00 00 00 02 C4 0B' : '输入要发送的文本'
                        }
                    />
                    <Flex justify="space-between" align="center" gap={8} wrap>
                        <span className="text-xs text-slate-500">
                            {debug.manual
                                ? changed
                                    ? '参数已修改，请先应用参数'
                                    : '自动采集已暂停，可以手动发送'
                                : '先暂停自动采集，再手动发送'}
                        </span>
                        <Space>
                            <Button onClick={onClose}>关闭</Button>
                            <Button
                                type="primary"
                                loading={debug.pending}
                                disabled={!ready || !debug.manual || !!changed || !input}
                                onClick={() =>
                                    run(() =>
                                        debug.send('write', {
                                            hex: serialPayloadHex(input, sendMode, ending),
                                        })
                                    )
                                }
                            >
                                发送
                            </Button>
                        </Space>
                    </Flex>
                </div>
            }
        >
            <Flex justify="space-between" align="center" gap={8} wrap>
                <Tag color={!ready ? 'default' : debug.manual ? 'orange' : 'green'}>
                    {debug.connection === 'connecting'
                        ? '正在连接'
                        : debug.connection === 'closed'
                          ? '已断开'
                          : debug.manual
                            ? '手动调试'
                            : '监听中'}
                </Tag>
                <Space wrap>
                    {debug.connection === 'closed' ? (
                        <Button onClick={debug.reconnect}>重新连接</Button>
                    ) : (
                        <Button
                            disabled={!ready}
                            loading={debug.pending}
                            onClick={() =>
                                run(() =>
                                    debug.manual
                                        ? debug.send('monitor')
                                        : debug.send('manual', debug.settings)
                                )
                            }
                        >
                            {debug.manual ? '恢复自动采集' : '暂停自动采集'}
                        </Button>
                    )}
                </Space>
            </Flex>
            {debug.notice && (
                <Alert
                    type={debug.connection === 'closed' ? 'warning' : 'info'}
                    showIcon
                    title={debug.notice}
                />
            )}
            <Form
                form={form}
                layout="vertical"
                initialValues={debug.settings}
                disabled={!ready || !debug.manual || debug.pending}
                className="shrink-0"
                onFinish={(raw) => {
                    const parsed = validateForm(form, serialSettingsSchema, raw);
                    if (parsed) run(() => debug.send('manual', parsed));
                }}
            >
                <div className="grid grid-cols-3 gap-x-3 sm:grid-cols-5">
                    <Form.Item label="波特率" name="baudRate" className="mb-2">
                        <Select
                            options={[
                                300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200,
                                230400, 460800,
                            ].map((value) => ({ value, label: String(value) }))}
                        />
                    </Form.Item>
                    <Form.Item label="数据位" name="dataBits" className="mb-2">
                        <Select
                            options={[5, 6, 7, 8].map((value) => ({ value, label: String(value) }))}
                        />
                    </Form.Item>
                    <Form.Item label="停止位" name="stopBits" className="mb-2">
                        <Select
                            options={[
                                { value: 1, label: '1' },
                                { value: 2, label: '2' },
                            ]}
                        />
                    </Form.Item>
                    <Form.Item label="校验" name="parity" className="mb-2">
                        <Select
                            options={[
                                { value: 'none', label: '无' },
                                { value: 'even', label: '偶校验' },
                                { value: 'odd', label: '奇校验' },
                            ]}
                        />
                    </Form.Item>
                    <Form.Item label="RS485" name="rs485" valuePropName="checked" className="mb-2">
                        <Switch />
                    </Form.Item>
                </div>
                {debug.manual && (
                    <Button
                        htmlType="submit"
                        size="small"
                        disabled={!changed}
                        loading={debug.pending}
                    >
                        应用串口参数
                    </Button>
                )}
            </Form>
            <Flex justify="space-between" align="center" gap={8} wrap className="shrink-0">
                <Space wrap>
                    <Select
                        aria-label="接收显示格式"
                        size="small"
                        value={display}
                        onChange={setDisplay}
                        options={[
                            { value: 'hex', label: 'HEX 显示' },
                            { value: 'text', label: '文本显示' },
                        ]}
                    />
                    <Switch
                        size="small"
                        checked={follow}
                        onChange={setFollow}
                        aria-label="自动滚动"
                    />
                    <span className="text-xs">自动滚动</span>
                    <Button size="small" onClick={debug.clear}>
                        清空记录
                    </Button>
                </Space>
                <span className="text-xs text-slate-500">
                    TX {debug.counts.tx} B · RX {debug.counts.rx} B
                    {debug.counts.dropped > 0 ? ` · 丢弃 ${debug.counts.dropped} B` : ''}
                </span>
            </Flex>
            <div
                ref={logRef}
                role="log"
                aria-label="串口收发记录"
                className="min-h-24 flex-1 overflow-auto rounded-md bg-slate-950 p-3 font-mono text-xs text-slate-100"
            >
                {debug.frames.length === 0 ? (
                    <span className="text-slate-400">等待串口数据…</span>
                ) : (
                    debug.frames.map((frame) => (
                        <div key={frame.id} className="mb-1 grid grid-cols-[auto_auto_1fr] gap-2">
                            <span className="text-slate-400">
                                {new Date(frame.timestamp).toLocaleTimeString('zh-CN', {
                                    hour12: false,
                                })}
                                .{String(frame.timestamp % 1000).padStart(3, '0')}
                            </span>
                            <span
                                className={
                                    frame.direction === 'TX' ? 'text-amber-300' : 'text-emerald-300'
                                }
                            >
                                {frame.direction}
                            </span>
                            <span className="whitespace-pre-wrap break-all">
                                {display === 'hex' ? frame.hex.match(/../g)?.join(' ') : frame.text}
                            </span>
                        </div>
                    ))
                )}
            </div>
            <span className="shrink-0 text-xs text-slate-500">
                保留最近 200 条记录。关闭调试或会话超时后，节点恢复自动采集。
            </span>
        </FormModal>
    );
}

function TerminalModal({
    nodeId,
    open,
    onClose,
}: {
    nodeId?: string;
    open: boolean;
    onClose: () => void;
}) {
    const [state, setState] = useState('正在连接…');
    const [connectionEnded, setConnectionEnded] = useState(false);
    const [connectionAttempt, setConnectionAttempt] = useState(0);
    const terminalHostRef = useRef<HTMLDivElement | null>(null);
    useEffect(() => {
        void connectionAttempt;
        const host = terminalHostRef.current;
        if (!open || !nodeId || !host) return;
        const targetNodeId = nodeId;
        setConnectionEnded(false);
        setState('正在启动终端…');
        let disposed = false;
        let ended = false;
        let ready = false;
        let sessionId: string | undefined;
        let release: (() => void) | undefined;
        let heartbeat: ReturnType<typeof setInterval> | undefined;
        let fitFrame: number | undefined;
        let inputTimer: ReturnType<typeof setTimeout> | undefined;
        let lastSize = '';
        let resizing = false;
        let pendingSize: { columns: number; rows: number } | undefined;
        let lastOutputSequence = 0;
        let pendingCloseReason: string | undefined;
        let sending = false;
        let rendering = false;
        let inputBytes = 0;
        let outputBytes = 0;
        const inputQueue: Uint8Array[] = [];
        const outputQueue: { bytes: Uint8Array; sequence: number }[] = [];
        const encoder = new TextEncoder();
        const terminal = new Terminal({
            cursorBlink: true,
            fontFamily: "'Cascadia Mono', 'SFMono-Regular', Consolas, 'Liberation Mono', monospace",
            fontSize: 14,
            lineHeight: 1.2,
            scrollback: 2000,
            theme: {
                background: '#020617',
                foreground: '#d1fae5',
                cursor: '#6ee7b7',
                cursorAccent: '#020617',
                selectionBackground: '#164e63',
                black: '#0f172a',
                brightBlack: '#64748b',
                green: '#34d399',
                brightGreen: '#6ee7b7',
            },
        });
        const fitAddon = new FitAddon();
        terminal.loadAddon(fitAddon);
        terminal.open(host);
        try {
            const webgl = new WebglAddon();
            webgl.onContextLoss(() => webgl.dispose());
            terminal.loadAddon(webgl);
        } catch {
            // The default renderer remains available without WebGL.
        }
        const openingTimeout = setTimeout(() => finish('终端启动超时'), 15000);
        function finish(reason: string) {
            if (ended) return;
            ended = true;
            ready = false;
            release?.();
            release = undefined;
            clearTimeout(openingTimeout);
            if (heartbeat !== undefined) clearInterval(heartbeat);
            if (inputTimer !== undefined) clearTimeout(inputTimer);
            inputQueue.length = 0;
            outputQueue.length = 0;
            inputBytes = 0;
            outputBytes = 0;
            terminal.options.disableStdin = true;
            if (sessionId) void closeTerminal(targetNodeId, sessionId).catch(() => undefined);
            if (!disposed) {
                setState(reason);
                setConnectionEnded(true);
            }
        }
        function fail(error: unknown) {
            finish(error instanceof Error ? error.message : '终端通信失败');
        }
        async function flushResize() {
            if (resizing || !ready || !sessionId || ended) return;
            resizing = true;
            try {
                while (!ended && pendingSize) {
                    const size = pendingSize;
                    pendingSize = undefined;
                    await resizeTerminal(targetNodeId, sessionId, size.columns, size.rows);
                }
            } catch (error) {
                fail(error);
            } finally {
                resizing = false;
            }
        }
        function scheduleFit() {
            if (disposed || ended || fitFrame !== undefined) return;
            fitFrame = window.requestAnimationFrame(() => {
                fitFrame = undefined;
                if (!host?.clientWidth || !host.clientHeight) return;
                fitAddon.fit();
                const size = `${terminal.cols}:${terminal.rows}`;
                if (ready && sessionId && lastSize !== size) {
                    lastSize = size;
                    pendingSize = { columns: terminal.cols, rows: terminal.rows };
                    void flushResize();
                }
            });
        }
        async function flushInput() {
            inputTimer = undefined;
            if (sending || !ready || !sessionId || ended) return;
            sending = true;
            try {
                while (!ended && inputQueue.length > 0) {
                    const next = inputQueue[0];
                    const chunk = next.slice(0, 16384);
                    if (chunk.length === next.length) inputQueue.shift();
                    else inputQueue[0] = next.slice(chunk.length);
                    inputBytes -= chunk.length;
                    await writeTerminal(targetNodeId, sessionId, chunk);
                }
            } catch (error) {
                fail(error);
            } finally {
                sending = false;
            }
        }
        function renderOutput() {
            if (disposed || ended || rendering) return;
            if (outputQueue.length === 0) {
                if (pendingCloseReason !== undefined) finish(pendingCloseReason);
                return;
            }
            const item = outputQueue.shift();
            if (!item || !sessionId) return;
            rendering = true;
            terminal.write(item.bytes, () => {
                if (disposed || ended || !sessionId) {
                    rendering = false;
                    return;
                }
                outputBytes -= item.bytes.length;
                const confirmation =
                    item.sequence > 0
                        ? acknowledgeTerminalOutput(targetNodeId, sessionId, item.sequence)
                        : Promise.resolve();
                void confirmation
                    .then(() => {
                        rendering = false;
                        renderOutput();
                    })
                    .catch(fail);
            });
        }
        const resizeObserver = new ResizeObserver(scheduleFit);
        resizeObserver.observe(host);
        const focus = () => {
            if (disposed || ended || document.visibilityState !== 'visible') return;
            scheduleFit();
            terminal.focus();
        };
        window.addEventListener('focus', focus);
        document.addEventListener('visibilitychange', focus);
        scheduleFit();
        const input = terminal.onData((data) => {
            if (ended) return;
            const bytes = encoder.encode(data);
            if (inputBytes + bytes.length > 256 * 1024) {
                finish('终端输入队列已满，连接已关闭，请重新连接');
                return;
            }
            inputQueue.push(bytes);
            inputBytes += bytes.length;
            if (inputTimer === undefined) inputTimer = setTimeout(() => void flushInput(), 8);
        });
        void openTerminal(nodeId, terminal.cols, terminal.rows)
            .then(({ id }) => {
                if (disposed || ended) {
                    void closeTerminal(nodeId, id).catch(() => undefined);
                    return;
                }
                sessionId = id;
                release = getTerminalEvents(nodeId, id).subscribe({
                    next: (value) => {
                        if (disposed || ended) return;
                        try {
                            for (const event of terminalEventsSchema.parse(value).events) {
                                if (event.kind === 'ready') {
                                    ready = true;
                                    clearTimeout(openingTimeout);
                                    setState('已连接');
                                    terminal.focus();
                                    scheduleFit();
                                    void flushInput();
                                } else if (event.kind === 'close') {
                                    ready = false;
                                    terminal.options.disableStdin = true;
                                    inputQueue.length = 0;
                                    inputBytes = 0;
                                    pendingCloseReason = event.reason || '终端已关闭';
                                    renderOutput();
                                    break;
                                } else {
                                    const bytes = Uint8Array.from(
                                        atob(event.content),
                                        (character) => character.charCodeAt(0)
                                    );
                                    if (
                                        bytes.length > 16384 ||
                                        (event.sequence > 0 &&
                                            event.sequence !== lastOutputSequence + 1)
                                    )
                                        throw new Error('终端数据顺序无效，请重新连接');
                                    if (event.sequence > 0) lastOutputSequence = event.sequence;
                                    if (outputBytes + bytes.length > 1024 * 1024)
                                        throw new Error(
                                            '终端输出超过处理能力，连接已关闭，请重新连接'
                                        );
                                    outputQueue.push({ bytes, sequence: event.sequence });
                                    outputBytes += bytes.length;
                                    renderOutput();
                                }
                            }
                        } catch (error) {
                            fail(error);
                        }
                    },
                    error: fail,
                });
                heartbeat = setInterval(() => {
                    if (!ended && !resizing) void keepTerminalAlive(nodeId, id).catch(fail);
                }, 20000);
            })
            .catch(fail);
        return () => {
            disposed = true;
            finish('终端已关闭');
            if (fitFrame !== undefined) window.cancelAnimationFrame(fitFrame);
            resizeObserver.disconnect();
            window.removeEventListener('focus', focus);
            document.removeEventListener('visibilitychange', focus);
            input.dispose();
            terminal.dispose();
        };
    }, [nodeId, open, connectionAttempt]);
    return (
        <Modal
            open={open}
            zIndex={EDGE_ACTION_MODAL_Z_INDEX}
            onCancel={onClose}
            footer={null}
            width="min(960px, 94vw)"
            title={
                <Space>
                    <span>{`Web 终端 · ${state}`}</span>
                    {connectionEnded && (
                        <Button
                            size="small"
                            onClick={() => setConnectionAttempt((value) => value + 1)}
                        >
                            重新连接
                        </Button>
                    )}
                </Space>
            }
            forceRender
            destroyOnHidden
            styles={{
                body: {
                    height: 'min(620px, calc(90dvh - 72px))',
                    padding: 0,
                    background: '#020617',
                },
            }}
            style={{ top: 'max(16px, 5dvh)', paddingBottom: 0 }}
        >
            <div
                className="h-full w-full overflow-hidden rounded-md bg-slate-950 p-3"
                role="application"
                aria-label="边缘节点终端"
            >
                <div ref={terminalHostRef} className="h-full min-h-0 w-full overflow-hidden" />
            </div>
        </Modal>
    );
}
export function EdgeNodePage() {
    const { has } = usePermissions();
    const canQuery = has('iot:edge:query');
    const canEdit = has('iot:edge:edit');
    const canConfig = has('iot:edge:config');
    const canFirmware = has('iot:edge:firmware');
    const canTerminal = has('iot:edge:terminal');
    const [searchText, setSearchText] = useState('');
    const scrollContainerRef = useRef<HTMLDivElement>(null);
    const [keyword, setKeyword] = useState('');
    const [status, setStatus] = useState<Edge.EnrollmentStatus>();
    const [selectedGroupId, setSelectedGroupId] = useState<string | null>(null);
    const [selectedId, setSelectedId] = useState<string>();
    const [detailTab, setDetailTab] = useState('networks');
    const [renamingNode, setRenamingNode] = useState<Edge.Node>();
    const [groupingNode, setGroupingNode] = useState<Edge.Node>();
    const [networkNode, setNetworkNode] = useState<Edge.Node>();
    const [networkDraft, setNetworkDraft] = useState<NetworkDraftItem[]>([]);
    const [networkRollbackTimeoutSec, setNetworkRollbackTimeoutSec] = useState(60);
    const [editingNetwork, setEditingNetwork] = useState<NetworkDraftItem>();
    const [firmwareNode, setFirmwareNode] = useState<Edge.Node>();
    const [terminalNode, setTerminalNode] = useState<Edge.Node>();
    const [serialDebug, setSerialDebug] = useState<{
        nodeId: string;
        path: string;
        title: string;
    }>();
    const [logLevel, setLogLevel] = useState<Edge.LogLevel>();
    const [nodeLogLevel, setNodeLogLevel] = useState<Edge.LogLevel>('info');
    const [networkOpen, setNetworkOpen] = useState(false);
    const [firmwareOpen, setFirmwareOpen] = useState(false);
    const [firmwareUploadProgress, setFirmwareUploadProgress] =
        useState<Edge.FirmwareUploadProgress>();
    const [terminalOpen, setTerminalOpen] = useState(false);
    const [networkForm] = Form.useForm<Edge.NetworkConfig>();
    const [firmwareForm] = Form.useForm<Edge.FirmwareUpgradeDto>();
    const [nameForm] = Form.useForm<Edge.NameDto>();
    const [groupForm] = Form.useForm<Edge.GroupDto>();
    const networkMode = Form.useWatch('mode', networkForm);
    const networkBridge = Form.useWatch('bridge', networkForm);
    const firmwareFile = Form.useWatch('file', firmwareForm);
    const { message, modal } = App.useApp();
    const eventScope: Edge.EventScope = {
        nodeId: selectedId,
        logs:
            selectedId && detailTab === 'events'
                ? { limit: 48, level: logLevel }
                : selectedId && detailTab === 'system'
                  ? { limit: 48, source: 'system' }
                  : undefined,
        vpn: Boolean(selectedId && detailTab === 'vpn' && has('iot:vpn:query')),
    };
    const {
        data,
        isLoading,
        isFetching,
        refetch,
        error: inventoryError,
    } = useEdgeInventory(canQuery, eventScope);
    const { data: edgeGroups = [] } = useEdgeGroupTree();
    const groupView = useMemo(
        () => buildEdgeNodeGroupView(edgeGroups, data ?? [], selectedGroupId, keyword, status),
        [edgeGroups, data, selectedGroupId, keyword, status]
    );
    const nodes = groupView.filtered;
    const scrollScope = `${selectedGroupId ?? 'all'}:${keyword}:${status ?? 'all'}`;
    useEffect(() => {
        void scrollScope;
        if (scrollContainerRef.current) scrollContainerRef.current.scrollTop = 0;
    }, [scrollScope]);
    const edgeGroupOptions = useMemo(() => groupSelectOptions(edgeGroups), [edgeGroups]);
    const {
        data: detail,
        isLoading: detailLoading,
        error: detailError,
        refetch: retryDetail,
    } = useEdgeDetail(eventScope);
    const {
        data: eventLogs,
        isFetching: eventLogsLoading,
        refetch: refreshEventLogs,
        error: eventLogsError,
    } = useEdgeLogs(
        eventScope,
        Boolean(
            selectedId && detailTab === 'events' && detail?.status.online && detail.capability.logs
        )
    );
    const {
        data: systemLogs,
        isFetching: systemLogsLoading,
        refetch: refreshSystemLogs,
        error: systemLogsError,
    } = useEdgeLogs(
        eventScope,
        Boolean(
            selectedId && detailTab === 'system' && detail?.status.online && detail.capability.logs
        )
    );
    const enrollment = useEnrollmentMutation();
    const edgeDelete = useEdgeDeleteMutation();
    const nodeName = useRenameEdgeNode();
    const nodeGroup = useAssignEdgeNodeGroup();
    const network = useConfigureEdgeNetwork();
    const deviceConfigSync = useDeviceConfigSyncMutation();
    const firmwareUpgrade = useFirmwareUpgradeMutation();
    const logLevelControl = useSetEdgeLogLevel();
    useEffect(() => {
        if (!selectedId) return;
        const exists = data?.some((node) => node.id === selectedId) ?? true;
        if (!exists) setSelectedId(undefined);
    }, [data, selectedId]);
    useEffect(() => {
        if (detail) setNodeLogLevel(detail.status.log?.level ?? 'info');
    }, [detail]);
    if (!canQuery) {
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询边缘节点的权限" />
            </PageContainer>
        );
    }
    const approveEnrollment = (node: Edge.Node) => {
        modal.confirm({
            zIndex: EDGE_ACTION_MODAL_Z_INDEX,
            title: `批准 IMEI ${node.imei} 注册？`,
            content: '批准后当前连接会立即转为已批准会话，节点将上报能力并自动同步当前设备配置。',
            onOk: () =>
                enrollment.mutateAsync({
                    id: node.id,
                    status: 'approved',
                    name: node.name || node.hostname,
                }),
        });
    };
    const deleteEnrollment = (node: Edge.Node) => {
        modal.confirm({
            zIndex: EDGE_ACTION_MODAL_Z_INDEX,
            title: `删除 IMEI ${node.imei} 的注册申请？`,
            content: '删除后当前连接会断开；节点再次连接时会重新进入待处理状态。',
            okButtonProps: { danger: true },
            okText: '删除',
            onOk: () =>
                edgeDelete.mutateAsync(node.id).then(() => {
                    if (selectedId === node.id) setSelectedId(undefined);
                }),
        });
    };
    const showNetworkManager = async (node: Edge.Node) => {
        const current = node.networks && node.interfaces ? node : await getEdgeDetail(node.id);
        setNetworkDraft(
            (current.networks ?? []).map((item) =>
                networkDraftFromReported(item, current.interfaces ?? [])
            )
        );
        setNetworkRollbackTimeoutSec(60);
        setEditingNetwork(undefined);
        setNetworkOpen(false);
        setNetworkNode(current);
    };
    const showNetworkEditor = (item?: NetworkDraftItem) => {
        const interfaces = networkNode?.interfaces ?? [];
        const firstDevice = physicalNetworkInterfaces(interfaces)[0]?.name;
        networkForm.setFieldsValue(
            item
                ? {
                      operation: 'upsert',
                      name: item.name,
                      mode: item.mode,
                      device: item.device,
                      bridge: item.bridge,
                      bridgePorts: item.bridgePorts,
                      ip: item.ip,
                      prefixLength: item.prefixLength,
                      gateway: item.gateway,
                  }
                : {
                      operation: 'upsert',
                      name: '',
                      mode: 'dhcp',
                      device: firstDevice,
                      bridge: false,
                      bridgePorts: [],
                      ip: '',
                      prefixLength: 0,
                      gateway: '',
                  }
        );
        setEditingNetwork(item);
        setNetworkOpen(true);
    };
    const submitNetworkDraft = () => {
        if (!networkNode) return;
        const parsed = networkSchema.safeParse({
            interfaces: networkDraft.map(
                ({ original, dirty, up, sourceName, ...item }): Edge.NetworkConfig => {
                    if (item.operation === 'delete') {
                        return { ...item, name: sourceName ?? item.name };
                    }
                    return sourceName && sourceName !== item.name
                        ? { ...item, previousName: sourceName }
                        : item;
                }
            ),
            rollbackTimeoutSec: networkRollbackTimeoutSec,
        });
        if (!parsed.success) {
            void message.error(parsed.error.issues[0]?.message ?? '网络配置校验失败');
            return;
        }
        network.mutate(
            { id: networkNode.id, data: parsed.data },
            {
                onSuccess: () => {
                    setNetworkNode(undefined);
                    setNetworkDraft([]);
                },
            }
        );
    };
    const showRename = (node: Edge.Node) => {
        nameForm.setFieldsValue({ name: node.name || node.hostname });
        setRenamingNode(node);
    };
    const showGroup = (node: Edge.Node) => {
        groupForm.setFieldsValue({ groupId: node.groupId || '' });
        setGroupingNode(node);
    };
    const showDetail = (node: Edge.Node) => {
        setDetailTab('networks');
        setLogLevel(undefined);
        setSelectedId(node.id);
    };
    const showFirmware = (node: Edge.Node) => {
        firmwareUpgrade.reset();
        setFirmwareUploadProgress(undefined);
        firmwareForm.resetFields();
        firmwareForm.setFieldsValue({ keepSettings: true });
        setFirmwareNode(node);
        setFirmwareOpen(true);
    };
    const interfaceColumns: ColumnsType<Edge.NetworkInterface> = [
        { title: '接口', dataIndex: 'name' },
        { title: 'MAC', dataIndex: 'mac', render: (value) => value || '-' },
        {
            title: 'IPv4',
            render: (_, item) => (item.ipv4 ? `${item.ipv4}/${item.prefixLength}` : '-'),
        },
        {
            title: '状态',
            dataIndex: 'up',
            render: (value) => (
                <Tag color={value ? 'success' : 'default'}>{value ? 'UP' : 'DOWN'}</Tag>
            ),
        },
        {
            title: '类型',
            render: (_, item) => (item.bridge ? `网桥 ${item.bridgePorts.join(', ')}` : '网口'),
        },
    ];
    const networkInfoColumns: ColumnsType<Edge.Network> = [
        { title: '逻辑接口', dataIndex: 'name', width: 120 },
        {
            title: '地址方式',
            dataIndex: 'mode',
            width: 100,
            render: (value: Edge.Network['mode']) =>
                value === 'dhcp' ? 'DHCP' : value === 'static' ? '静态 IPv4' : '-',
        },
        {
            title: '网卡 / 网桥成员',
            render: (_, item) =>
                item.bridge ? item.bridgePorts.join(', ') || '-' : item.device || '-',
        },
        {
            title: 'IPv4',
            render: (_, item) => (item.ipv4 ? `${item.ipv4}/${item.prefixLength}` : '-'),
        },
        {
            title: '状态',
            dataIndex: 'up',
            width: 80,
            render: (value) => (
                <Tag color={value ? 'success' : 'default'}>{value ? 'UP' : 'DOWN'}</Tag>
            ),
        },
    ];
    const networkDraftColumns: ColumnsType<NetworkDraftItem> = [
        {
            title: '逻辑接口',
            dataIndex: 'name',
            width: 160,
            render: (name: string, item) =>
                item.sourceName && item.sourceName !== name ? `${item.sourceName} → ${name}` : name,
        },
        {
            title: '地址方式',
            dataIndex: 'mode',
            width: 100,
            render: (value: NetworkDraftItem['mode'], item) =>
                item.operation === 'delete'
                    ? '-'
                    : value === 'dhcp'
                      ? 'DHCP'
                      : value === 'static'
                        ? '静态 IPv4'
                        : '-',
        },
        {
            title: '网卡 / 网桥成员',
            render: (_, item) =>
                item.bridge ? item.bridgePorts?.join(', ') || '-' : item.device || '-',
        },
        {
            title: 'IPv4',
            render: (_, item) =>
                item.operation !== 'delete' && item.ip ? `${item.ip}/${item.prefixLength}` : '-',
        },
        {
            title: '修改状态',
            width: 100,
            render: (_, item) => {
                if (item.operation === 'delete') return <Tag color="error">待删除</Tag>;
                if (!item.original) return <Tag color="success">待新增</Tag>;
                if (item.dirty) return <Tag color="processing">待更新</Tag>;
                return <Tag>未修改</Tag>;
            },
        },
        {
            title: '操作',
            width: 140,
            render: (_, item) => (
                <Space size={4}>
                    {item.operation === 'delete' ? (
                        <Button
                            type="link"
                            size="small"
                            onClick={() => {
                                const sourceName = item.sourceName ?? item.name;
                                const original = networkNode?.networks?.find(
                                    (candidate) => candidate.name === sourceName
                                );
                                if (!original) return;
                                setNetworkDraft((current) =>
                                    current.map((candidate) =>
                                        (candidate.sourceName ?? candidate.name) === sourceName
                                            ? networkDraftFromReported(
                                                  original,
                                                  networkNode?.interfaces ?? []
                                              )
                                            : candidate
                                    )
                                );
                            }}
                        >
                            恢复
                        </Button>
                    ) : (
                        <>
                            <Button
                                type="link"
                                size="small"
                                onClick={() => showNetworkEditor(item)}
                            >
                                编辑
                            </Button>
                            <Popconfirm
                                title={`将逻辑接口 ${item.name} 标记为待删除？`}
                                description="这里只修改草稿，点击“保存并下发全部配置”后节点才会应用。"
                                onConfirm={() =>
                                    setNetworkDraft((current) =>
                                        item.original
                                            ? current.map((candidate) =>
                                                  (candidate.sourceName ?? candidate.name) ===
                                                  (item.sourceName ?? item.name)
                                                      ? {
                                                            ...candidate,
                                                            operation: 'delete',
                                                            dirty: true,
                                                        }
                                                      : candidate
                                              )
                                            : current.filter(
                                                  (candidate) =>
                                                      (candidate.sourceName ?? candidate.name) !==
                                                      item.name
                                              )
                                    )
                                }
                            >
                                <Button type="link" size="small" danger>
                                    删除
                                </Button>
                            </Popconfirm>
                        </>
                    )}
                </Space>
            ),
        },
    ];
    const assignedNetworkDevices = new Set(
        networkDraft
            .filter(
                (item) =>
                    item.operation === 'upsert' &&
                    (item.sourceName ?? item.name) !==
                        (editingNetwork?.sourceName ?? editingNetwork?.name)
            )
            .flatMap((item) => (item.bridge ? item.bridgePorts : item.device ? [item.device] : []))
    );
    const networkInterfaces = networkNode?.interfaces ?? [];
    const networkDeviceOptions = physicalNetworkInterfaces(networkInterfaces).map((item) => ({
        value: item.name,
        label: item.displayName ? `${item.displayName} (${item.name})` : item.name,
        disabled: assignedNetworkDevices.has(item.name),
    }));
    const serialColumns: ColumnsType<Edge.SerialPort> = [
        { title: '串口', dataIndex: 'path' },
        { title: '名称', dataIndex: 'displayName' },
        { title: '可读写', dataIndex: 'available', render: (value) => (value ? '是' : '否') },
        { title: 'RS485', dataIndex: 'rs485', render: (value) => (value ? '是' : '未确认') },
        {
            title: '操作',
            width: 100,
            render: (_, port) =>
                canTerminal && (
                    <Tooltip
                        title={
                            !detail?.capability.serialDebug
                                ? '需要支持串口调试的新版固件'
                                : !detail.status.online
                                  ? '节点离线'
                                  : !port.available
                                    ? '串口不可用'
                                    : undefined
                        }
                    >
                        <Button
                            type="link"
                            size="small"
                            disabled={
                                !detail?.capability.serialDebug ||
                                !detail.status.online ||
                                !port.available
                            }
                            onClick={() =>
                                detail &&
                                setSerialDebug({
                                    nodeId: detail.id,
                                    path: port.path,
                                    title: detail.name || detail.imei,
                                })
                            }
                        >
                            串口调试
                        </Button>
                    </Tooltip>
                ),
        },
    ];
    const taskColumns: ColumnsType<Edge.Task> = [
        { title: '类型', dataIndex: 'taskType' },
        { title: '状态', dataIndex: 'status', render: statusTag },
        {
            title: '进度',
            width: 220,
            render: (_, item) =>
                item.taskType === 'firmware' ? (
                    <Progress
                        percent={item.progressPercent}
                        size="small"
                        status={
                            item.status === 'failed'
                                ? 'exception'
                                : item.status === 'succeeded'
                                  ? 'success'
                                  : 'active'
                        }
                        format={(percent) =>
                            `${percent ?? 0}% · ${formatBytes(item.downloadedBytes)} / ${formatBytes(item.totalBytes)}`
                        }
                    />
                ) : (
                    '-'
                ),
        },
        { title: '结果', dataIndex: 'message', render: (value) => value || '-' },
        {
            title: '创建时间',
            dataIndex: 'createdAt',
            render: (value) => formatDateTime(value),
        },
    ];
    const logColumns: ColumnsType<Edge.LogLine> = [
        {
            title: '时间',
            dataIndex: 'time',
            width: 170,
            render: (value) => formatDateTime(value),
        },
        {
            title: '级别',
            dataIndex: 'level',
            width: 90,
            render: logLevelTag,
        },
        { title: '来源', dataIndex: 'source', width: 110, render: (value) => value || '-' },
        { title: '事件', dataIndex: 'message', width: 180, render: (value) => value || '-' },
        { title: '详情', dataIndex: 'detail', render: (value) => value || '-' },
    ];
    const renderNodeCards = (items: Edge.Node[]) => (
        <div className="mt-4">
            <div className={EDGE_CARD_GRID_CLASS}>
                {items.map((node) => {
                    const status = node.status;
                    const nodeTitle = node.name || node.hostname || '未命名节点';
                    return (
                        <div key={node.id} className="flex flex-col">
                            <DeviceCard
                                onClick={() => showDetail(node)}
                                ariaLabel={`查看边缘节点 ${node.name || node.hostname || node.imei}`}
                                title={
                                    <Flex
                                        justify="space-between"
                                        align="start"
                                        gap={10}
                                        className="w-full min-w-0"
                                    >
                                        <Tooltip title={`${nodeTitle} · IMEI：${node.imei}`}>
                                            <span className="min-w-0 flex-1 truncate whitespace-nowrap pr-1 text-left leading-5">
                                                {nodeTitle}
                                                <span className="ml-2 text-xs font-normal text-slate-400">
                                                    IMEI：{node.imei}
                                                </span>
                                            </span>
                                        </Tooltip>
                                        <Tag
                                            color={status.online ? 'success' : 'default'}
                                            className="!mr-0 shrink-0 !rounded-md !px-2"
                                        >
                                            {status.online ? '在线' : '离线'}
                                        </Tag>
                                    </Flex>
                                }
                                subtitle={
                                    <div className="flex w-full min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
                                        <span className="flex min-w-0 shrink-0 items-center">
                                            <Tag color="blue" className="!mr-0 !rounded-md">
                                                {node.model || '未知型号'}
                                            </Tag>
                                            <Tag color="purple" className="!mr-0 !rounded-md">
                                                {node.softwareVersion || '未知版本'}
                                            </Tag>
                                        </span>
                                        <span className="min-w-0 truncate text-xs text-slate-400">
                                            上报：{formatDateTime(status.lastSeenAt)}
                                        </span>
                                    </div>
                                }
                                items={buildNodeCardItems(node)}
                                column={8}
                                extra={
                                    <Flex
                                        align="center"
                                        justify="center"
                                        gap={6}
                                        className="w-full text-slate-500"
                                    >
                                        <EyeOutlined />
                                        <span>查看详情</span>
                                    </Flex>
                                }
                            />
                        </div>
                    );
                })}
            </div>
        </div>
    );
    const renderSectionStats = (
        items: Edge.Node[],
        aggregate?: {
            total: number;
            online: number;
            offline: number;
        }
    ) => {
        const stats = aggregate ?? {
            total: items.length,
            online: items.filter((node) => node.status.online).length,
            offline: items.filter((node) => !node.status.online).length,
        };
        return (
            <Space size={6} wrap>
                <Tag color="blue">{stats.total} 个</Tag>
                {stats.online > 0 && <Tag color="green">{stats.online} 在线</Tag>}
                {stats.offline > 0 && <Tag color="red">{stats.offline} 离线</Tag>}
            </Space>
        );
    };
    const renderGroupSection = (group: Edge.GroupTreeItem, depth = 0): ReactNode => {
        const stats = groupView.stats.get(group.id);
        if (!stats?.total) return null;
        const direct = groupView.direct.get(group.id) ?? [];
        const children = (group.children ?? []).filter(
            (child) => (groupView.stats.get(child.id)?.total ?? 0) > 0
        );
        return (
            <section
                key={group.id}
                className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4 [margin-left:var(--group-depth-offset)]"
                style={
                    {
                        '--group-depth-offset': depth > 0 ? `${depth * 16}px` : '0px',
                    } as CSSProperties
                }
            >
                <Flex justify="space-between" align="center" gap={12} wrap>
                    <div className="text-sm font-semibold text-slate-800">{group.name}</div>
                    {renderSectionStats(direct, stats)}
                </Flex>
                {direct.length > 0 && renderNodeCards(direct)}
                {children.length > 0 && (
                    <Space direction="vertical" className="mt-4 w-full" size="middle">
                        {children.map((child) => renderGroupSection(child, depth + 1))}
                    </Space>
                )}
            </section>
        );
    };
    const renderUngroupedSection = (items: Edge.Node[]) =>
        items.length > 0 && (
            <section className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4">
                <Flex justify="space-between" align="center" gap={12} wrap>
                    <div className="min-w-0">
                        <div className="text-sm font-semibold text-slate-800">未分组</div>
                        <div className="mt-1 text-xs text-slate-500">
                            没有绑定节点分组的卡片会统一在这里展示
                        </div>
                    </div>
                    {renderSectionStats(items)}
                </Flex>
                {renderNodeCards(items)}
            </section>
        );
    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-center justify-between gap-2">
                    <h3 className="m-0 text-base font-medium">边缘节点</h3>
                    <Space wrap>
                        <Button
                            icon={<DownloadOutlined />}
                            href={getWindowsClientDownloadUrl()}
                            download
                        >
                            下载 Windows 客户端
                        </Button>
                        <EdgeNodeGroupPanel
                            selectedGroupId={selectedGroupId}
                            canManageGroup={canEdit}
                            onSelect={setSelectedGroupId}
                            ungroupedCount={groupView.ungroupedCount}
                            nodes={data ?? []}
                        />
                        <Input.Search
                            allowClear
                            className="w-[240px]"
                            placeholder="IMEI / 名称 / 型号"
                            enterButton
                            value={searchText}
                            onChange={(event) => {
                                setSearchText(event.target.value);
                                if (!event.target.value) setKeyword('');
                            }}
                            onSearch={(value) => setKeyword(value.trim())}
                        />
                        <Select
                            allowClear
                            className="w-[130px]"
                            placeholder="注册状态"
                            value={status}
                            onChange={setStatus}
                            options={[
                                { value: 'pending', label: '待处理' },
                                { value: 'approved', label: '已批准' },
                            ]}
                        />
                        <Tooltip title="刷新">
                            <Button
                                icon={<ReloadOutlined />}
                                loading={isFetching}
                                onClick={() => void refetch()}
                            />
                        </Tooltip>
                    </Space>
                </div>
            }
        >
            <LiveQueryError
                error={
                    inventoryError ??
                    detailError ??
                    (detailTab === 'events'
                        ? eventLogsError
                        : detailTab === 'system'
                          ? systemLogsError
                          : null)
                }
                retry={() =>
                    Promise.all([
                        refetch(),
                        ...(selectedId ? [retryDetail()] : []),
                        ...(eventLogsError && detailTab === 'events' ? [refreshEventLogs()] : []),
                        ...(systemLogsError && detailTab === 'system' ? [refreshSystemLogs()] : []),
                    ])
                }
                loading={isFetching}
            />
            <div ref={scrollContainerRef} className="h-full overflow-y-auto overflow-x-hidden">
                {isLoading && nodes.length === 0 ? (
                    <div className={EDGE_CARD_GRID_CLASS}>
                        {['first', 'second', 'third', 'fourth'].map((key) => (
                            <div key={key} className="rounded-lg bg-white px-3.5 py-3">
                                <Skeleton active paragraph={{ rows: 4 }} />
                            </div>
                        ))}
                    </div>
                ) : nodes.length === 0 ? (
                    <div className="py-16">
                        <Empty description={keyword ? '未找到匹配的边缘节点' : '暂无边缘节点'} />
                    </div>
                ) : (
                    <Space direction="vertical" className="w-full" size="large">
                        {groupView.roots.length > 0 ? (
                            <>
                                {groupView.roots.map((group) => renderGroupSection(group))}
                                {selectedGroupId === null &&
                                    renderUngroupedSection(groupView.ungrouped)}
                            </>
                        ) : selectedGroupId === 'ungrouped' ? (
                            renderUngroupedSection(nodes)
                        ) : (
                            <section className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4">
                                <Flex justify="space-between" align="center" gap={12} wrap>
                                    <div className="text-sm font-semibold text-slate-800">
                                        全部节点
                                    </div>
                                    {renderSectionStats(nodes)}
                                </Flex>
                                {renderNodeCards(nodes)}
                            </section>
                        )}
                    </Space>
                )}
            </div>

            <Drawer
                open={Boolean(selectedId)}
                zIndex={EDGE_DETAIL_DRAWER_Z_INDEX}
                onClose={() => setSelectedId(undefined)}
                title={
                    detail
                        ? `${detail.name || detail.hostname || '边缘节点'} · ${detail.imei}`
                        : '边缘节点详情'
                }
                extra={
                    detail ? (
                        <Space>
                            {canEdit && (
                                <>
                                    <Button
                                        icon={<EditOutlined />}
                                        onClick={() => showRename(detail)}
                                    >
                                        修改名称
                                    </Button>
                                    <Button
                                        icon={<ApartmentOutlined />}
                                        onClick={() => showGroup(detail)}
                                    >
                                        设置分组
                                    </Button>
                                </>
                            )}
                            {detail.enrollmentStatus === 'approved' &&
                                canTerminal &&
                                detail.capability.terminal && (
                                    <Tooltip
                                        title={detail.status.online ? '打开 Web 终端' : '节点离线'}
                                    >
                                        <span>
                                            <Button
                                                icon={<CodeOutlined />}
                                                disabled={!detail.status.online}
                                                onClick={() => {
                                                    setTerminalNode(detail);
                                                    setTerminalOpen(true);
                                                }}
                                            >
                                                Web 终端
                                            </Button>
                                        </span>
                                    </Tooltip>
                                )}
                        </Space>
                    ) : null
                }
                footer={
                    detail?.enrollmentStatus === 'pending' && canEdit ? (
                        <Flex justify="end" gap={8}>
                            <Button
                                danger
                                icon={<DeleteOutlined />}
                                loading={edgeDelete.isPending && edgeDelete.variables === detail.id}
                                onClick={() => deleteEnrollment(detail)}
                            >
                                删除注册申请
                            </Button>
                            <Button
                                type="primary"
                                icon={<CheckOutlined />}
                                loading={
                                    enrollment.isPending && enrollment.variables?.id === detail.id
                                }
                                onClick={() => approveEnrollment(detail)}
                            >
                                批准注册
                            </Button>
                        </Flex>
                    ) : undefined
                }
                size="min(960px, 92vw)"
                loading={detailLoading}
            >
                {detail && (
                    <>
                        <Descriptions bordered size="small" column={{ xs: 1, sm: 2, lg: 3 }}>
                            <Descriptions.Item label="IMEI">{detail.imei}</Descriptions.Item>
                            <Descriptions.Item label="状态">
                                {statusTag(detail.enrollmentStatus)}{' '}
                                {detail.status.online ? (
                                    <Tag color="success">在线</Tag>
                                ) : (
                                    <Tag>离线</Tag>
                                )}
                            </Descriptions.Item>
                            <Descriptions.Item label="型号">
                                {detail.model || '-'}
                            </Descriptions.Item>
                            <Descriptions.Item label="软件版本">
                                {detail.softwareVersion || '-'}
                            </Descriptions.Item>
                            <Descriptions.Item label="架构">
                                {detail.architecture || '-'}
                            </Descriptions.Item>
                            <Descriptions.Item label="OpenWrt">
                                {detail.openwrtRelease || '-'}
                            </Descriptions.Item>
                            <Descriptions.Item label="待上报队列">
                                {detail.status.outbox.records} 条 /{' '}
                                {formatBytes(detail.status.outbox.bytes)}
                            </Descriptions.Item>
                            <Descriptions.Item label="ttyd">
                                {detail.capability.terminal ? (
                                    <Tag color="success">已检测</Tag>
                                ) : (
                                    <Tag>未安装</Tag>
                                )}
                            </Descriptions.Item>
                            <Descriptions.Item label="最近上报">
                                {formatDateTime(detail.status.lastSeenAt)}
                            </Descriptions.Item>
                        </Descriptions>
                        <Tabs
                            className="mt-4"
                            activeKey={detailTab}
                            onChange={setDetailTab}
                            items={[
                                {
                                    key: 'networks',
                                    label: `逻辑接口 (${detail.networks?.length ?? 0})`,
                                    children: (
                                        <>
                                            <Flex
                                                justify="space-between"
                                                align="center"
                                                gap={12}
                                                className="mb-3"
                                            >
                                                <span className="text-xs text-slate-500">
                                                    查看逻辑接口及其物理网卡绑定关系。
                                                </span>
                                                {detail.enrollmentStatus === 'approved' &&
                                                    canConfig && (
                                                        <Tooltip
                                                            title={
                                                                detail.capability.networkConfig &&
                                                                detail.capability
                                                                    .networkConfigVersion >= 2
                                                                    ? '集中编辑并原子下发网络配置'
                                                                    : `节点代理 ${detail.softwareVersion || '当前版本'} 过旧，请升级至 0.3.0`
                                                            }
                                                        >
                                                            <span>
                                                                <Button
                                                                    icon={<GlobalOutlined />}
                                                                    disabled={
                                                                        !detail.capability
                                                                            .networkConfig ||
                                                                        detail.capability
                                                                            .networkConfigVersion <
                                                                            2
                                                                    }
                                                                    onClick={() =>
                                                                        void showNetworkManager(
                                                                            detail
                                                                        )
                                                                    }
                                                                >
                                                                    管理网络接口
                                                                </Button>
                                                            </span>
                                                        </Tooltip>
                                                    )}
                                            </Flex>
                                            <Table
                                                rowKey="name"
                                                size="small"
                                                pagination={false}
                                                columns={networkInfoColumns}
                                                dataSource={detail.networks ?? []}
                                                scroll={{ x: 'max-content', y: 360 }}
                                            />
                                        </>
                                    ),
                                },
                                {
                                    key: 'interfaces',
                                    label: `物理网卡 (${physicalNetworkInterfaces(detail.interfaces ?? []).length})`,
                                    children: (
                                        <Table
                                            rowKey="name"
                                            size="small"
                                            pagination={false}
                                            columns={interfaceColumns}
                                            dataSource={physicalNetworkInterfaces(
                                                detail.interfaces ?? []
                                            )}
                                            scroll={{ x: 'max-content', y: 360 }}
                                        />
                                    ),
                                },
                                {
                                    key: 'serial',
                                    label: `串口 (${detail.serialPorts?.length ?? 0})`,
                                    children: (
                                        <Table
                                            rowKey="path"
                                            size="small"
                                            pagination={false}
                                            columns={serialColumns}
                                            dataSource={detail.serialPorts ?? []}
                                            scroll={{ x: 'max-content', y: 360 }}
                                        />
                                    ),
                                },
                                {
                                    key: 'config',
                                    label: '设备配置',
                                    children: (
                                        <>
                                            <Flex
                                                justify="space-between"
                                                align="center"
                                                gap={12}
                                                className="mb-3"
                                            >
                                                <span className="text-xs text-slate-500">
                                                    查看平台目标版本与节点实际应用版本。
                                                </span>
                                                {detail.enrollmentStatus === 'approved' &&
                                                    canConfig &&
                                                    detail.capability.deviceConfig && (
                                                        <Button
                                                            icon={<SyncOutlined />}
                                                            loading={
                                                                deviceConfigSync.isPending &&
                                                                deviceConfigSync.variables ===
                                                                    detail.id
                                                            }
                                                            onClick={() =>
                                                                deviceConfigSync.mutate(detail.id)
                                                            }
                                                        >
                                                            同步设备配置
                                                        </Button>
                                                    )}
                                            </Flex>
                                            <Descriptions bordered size="small" column={2}>
                                                <Descriptions.Item label="状态">
                                                    {statusTag(detail.status.config.state)}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="配置版本">
                                                    {formatConfigVersions(
                                                        detail.status.config.activeVersion,
                                                        detail.status.config.desiredVersion
                                                    )}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="节点已应用版本">
                                                    {validConfigTimestamp(
                                                        detail.status.config.activeVersion
                                                    )
                                                        ? detail.status.config.activeVersion
                                                        : '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="平台目标版本">
                                                    {validConfigTimestamp(
                                                        detail.status.config.desiredVersion
                                                    )
                                                        ? detail.status.config.desiredVersion
                                                        : '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="结果" span={2}>
                                                    {detail.status.config.message || '-'}
                                                </Descriptions.Item>
                                            </Descriptions>
                                        </>
                                    ),
                                },
                                {
                                    key: 'mobile',
                                    label: '移动网络',
                                    children: (
                                        <>
                                            <p className="mb-3 text-xs text-slate-500">
                                                查看 SIM、信号、运营商与拨号状态。
                                            </p>
                                            <Descriptions bordered size="small" column={2}>
                                                <Descriptions.Item label="4G 状态">
                                                    {mobileState(detail)}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="SIM 状态">
                                                    {simStateText(detail.mobile.simState)}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="ICCID">
                                                    {detail.mobile.iccid || '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="运营商">
                                                    {detail.mobile.operator || '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="信号">
                                                    {detail.mobile.available
                                                        ? `${detail.mobile.signal.percent}%${
                                                              detail.mobile.signal.rssiDbm !== -1
                                                                  ? ` · ${detail.mobile.signal.rssiDbm} dBm`
                                                                  : ''
                                                          }`
                                                        : '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="APN">
                                                    {detail.mobile.apn || '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="网络注册">
                                                    {detail.mobile.registered ? '已注册' : '未注册'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="IPv4">
                                                    {detail.mobile.ipv4 || '-'}
                                                </Descriptions.Item>
                                            </Descriptions>
                                        </>
                                    ),
                                },
                                {
                                    key: 'vpn',
                                    label: 'VPN',
                                    children: <EdgeVpnPanel node={detail} scope={eventScope} />,
                                },
                                {
                                    key: 'tasks',
                                    label: '任务记录',
                                    children: (
                                        <Table
                                            rowKey="id"
                                            size="small"
                                            pagination={false}
                                            columns={taskColumns}
                                            dataSource={(detail.tasks ?? []).filter(
                                                (item) => item.taskType !== 'firmware'
                                            )}
                                            scroll={{ x: 'max-content', y: 360 }}
                                            locale={{ emptyText: '暂无任务记录' }}
                                        />
                                    ),
                                },
                                {
                                    key: 'firmware',
                                    label: '固件',
                                    children: (
                                        <>
                                            <Flex
                                                justify="space-between"
                                                align="center"
                                                gap={12}
                                                className="mb-3"
                                            >
                                                <span className="text-xs text-slate-500">
                                                    查看当前版本、刷写进度与历史升级记录。
                                                </span>
                                                {detail.enrollmentStatus === 'approved' &&
                                                    canFirmware &&
                                                    detail.capability.firmwareUpdate && (
                                                        <Button
                                                            danger
                                                            icon={<UploadOutlined />}
                                                            onClick={() => showFirmware(detail)}
                                                        >
                                                            上传固件并刷写
                                                        </Button>
                                                    )}
                                            </Flex>
                                            <Descriptions
                                                bordered
                                                size="small"
                                                column={2}
                                                className="mb-3"
                                            >
                                                <Descriptions.Item label="当前版本">
                                                    {detail.softwareVersion || '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="升级状态">
                                                    {detail.firmware.state
                                                        ? statusTag(detail.firmware.state)
                                                        : '-'}
                                                </Descriptions.Item>
                                                <Descriptions.Item label="当前进度" span={2}>
                                                    {detail.firmware.state === 'accepted' ||
                                                    detail.firmware.state === 'running' ? (
                                                        <Progress
                                                            percent={
                                                                detail.firmware.progressPercent
                                                            }
                                                            size="small"
                                                            status="active"
                                                            format={(percent) =>
                                                                `${percent ?? 0}% · ${formatBytes(detail.firmware.downloadedBytes)} / ${formatBytes(detail.firmware.totalBytes)}`
                                                            }
                                                        />
                                                    ) : (
                                                        detail.firmware.message || '-'
                                                    )}
                                                </Descriptions.Item>
                                            </Descriptions>
                                            <Table
                                                rowKey="id"
                                                size="small"
                                                pagination={false}
                                                columns={taskColumns}
                                                dataSource={(detail.tasks ?? []).filter(
                                                    (item) => item.taskType === 'firmware'
                                                )}
                                                locale={{ emptyText: '暂无固件升级记录' }}
                                                scroll={{ x: 'max-content', y: 300 }}
                                            />
                                        </>
                                    ),
                                },
                                {
                                    key: 'events',
                                    label: '运行事件',
                                    children: !detail.status.online ? (
                                        <Empty description="节点当前离线，无法读取运行事件" />
                                    ) : !detail.capability.logs ? (
                                        <Empty
                                            description={`节点代理 ${detail.softwareVersion || '当前版本'} 过旧，请升级后查看运行事件`}
                                        />
                                    ) : (
                                        <>
                                            <Flex
                                                justify="space-between"
                                                align="center"
                                                gap={12}
                                                className="mb-3"
                                            >
                                                <Space>
                                                    <Select<Edge.LogLevel>
                                                        className="w-[140px]"
                                                        value={nodeLogLevel}
                                                        loading={logLevelControl.isPending}
                                                        disabled={!canConfig}
                                                        onChange={(value) => {
                                                            setNodeLogLevel(value);
                                                            if (selectedId)
                                                                logLevelControl.mutate(
                                                                    {
                                                                        id: selectedId,
                                                                        data: { level: value },
                                                                    },
                                                                    {
                                                                        onSuccess: () =>
                                                                            void refreshEventLogs(),
                                                                    }
                                                                );
                                                        }}
                                                        options={[
                                                            { value: 'debug', label: 'DEBUG' },
                                                            { value: 'info', label: 'INFO' },
                                                            { value: 'warn', label: 'WARN' },
                                                            { value: 'error', label: 'ERROR' },
                                                        ]}
                                                    />
                                                    <Select<Edge.LogLevel>
                                                        allowClear
                                                        className="w-[140px]"
                                                        placeholder="筛选级别"
                                                        value={logLevel}
                                                        onChange={(value) => setLogLevel(value)}
                                                        options={[
                                                            { value: 'debug', label: 'debug' },
                                                            { value: 'info', label: 'info' },
                                                            { value: 'warn', label: 'warn' },
                                                            { value: 'error', label: 'error' },
                                                        ]}
                                                    />
                                                </Space>
                                                <Button
                                                    icon={<ReloadOutlined />}
                                                    loading={eventLogsLoading}
                                                    onClick={() => void refreshEventLogs()}
                                                >
                                                    刷新
                                                </Button>
                                            </Flex>
                                            <Table
                                                rowKey={(item, index) =>
                                                    `${item.time}-${item.source}-${index ?? 0}`
                                                }
                                                size="small"
                                                pagination={false}
                                                loading={eventLogsLoading}
                                                columns={logColumns}
                                                dataSource={eventLogs?.lines ?? []}
                                                locale={{ emptyText: '暂无运行事件' }}
                                                scroll={{ x: 'max-content', y: 420 }}
                                            />
                                        </>
                                    ),
                                },
                                {
                                    key: 'system',
                                    label: '系统日志',
                                    children: !detail.status.online ? (
                                        <Empty description="节点当前离线，无法读取系统日志" />
                                    ) : !detail.capability.logs ? (
                                        <Empty
                                            description={`节点代理 ${detail.softwareVersion || '当前版本'} 过旧，请升级后查看系统日志`}
                                        />
                                    ) : (
                                        <>
                                            <Flex justify="end" className="mb-3">
                                                <Button
                                                    icon={<ReloadOutlined />}
                                                    loading={systemLogsLoading}
                                                    onClick={() => void refreshSystemLogs()}
                                                >
                                                    刷新
                                                </Button>
                                            </Flex>
                                            <Table
                                                rowKey={(item, index) =>
                                                    `${item.time}-${item.message}-${index ?? 0}`
                                                }
                                                size="small"
                                                pagination={false}
                                                loading={systemLogsLoading}
                                                columns={logColumns}
                                                dataSource={systemLogs?.lines ?? []}
                                                locale={{
                                                    emptyText:
                                                        '暂无系统日志；节点版本低于 0.3.29 时请先升级代理',
                                                }}
                                                scroll={{ x: 'max-content', y: 420 }}
                                            />
                                        </>
                                    ),
                                },
                            ]}
                        />
                    </>
                )}
            </Drawer>

            <FormModal
                open={Boolean(networkNode)}
                zIndex={EDGE_ACTION_MODAL_Z_INDEX}
                onCancel={() => {
                    if (networkOpen) {
                        setNetworkOpen(false);
                        setEditingNetwork(undefined);
                    } else {
                        setNetworkNode(undefined);
                        setNetworkDraft([]);
                    }
                }}
                onOk={() => (networkOpen ? networkForm.submit() : submitNetworkDraft())}
                okText={networkOpen ? '保存修改' : '保存并下发全部配置'}
                cancelText={networkOpen ? '返回接口列表' : '取消'}
                confirmLoading={!networkOpen && network.isPending}
                okButtonProps={{
                    disabled: !networkOpen && !networkDraft.some((item) => item.dirty),
                }}
                title={`${networkOpen ? (editingNetwork ? '编辑网络接口' : '添加网络接口') : '网络接口'}${networkNode ? ` · ${networkNode.name || networkNode.imei}` : ''}`}
                forceRender
                destroyOnHidden
            >
                {networkOpen ? (
                    <Form
                        form={networkForm}
                        layout="vertical"
                        onFinish={(values) => {
                            const parsed = validateForm(
                                networkForm,
                                networkInterfaceSchema,
                                values
                            );
                            if (!parsed) return;
                            const draft: NetworkDraftItem = {
                                ...parsed,
                                operation: 'upsert',
                                sourceName: editingNetwork?.sourceName,
                                original: editingNetwork?.original ?? false,
                                dirty: true,
                                up: editingNetwork?.up,
                            };
                            setNetworkDraft((current) =>
                                editingNetwork
                                    ? current.map((item) =>
                                          (item.sourceName ?? item.name) ===
                                          (editingNetwork.sourceName ?? editingNetwork.name)
                                              ? draft
                                              : item
                                      )
                                    : [...current, draft]
                            );
                            setNetworkOpen(false);
                            setEditingNetwork(undefined);
                        }}
                    >
                        <Form.Item name="operation" hidden>
                            <Input />
                        </Form.Item>
                        <div className="grid grid-cols-1 gap-x-4 sm:grid-cols-2">
                            <Form.Item label="逻辑接口名称" name="name">
                                <Input maxLength={15} placeholder="例如 lan" />
                            </Form.Item>
                            <Form.Item label="地址方式" name="mode">
                                <Select
                                    options={[
                                        { value: 'dhcp', label: 'DHCP 客户端' },
                                        { value: 'static', label: '静态 IPv4' },
                                    ]}
                                    onChange={(value) => {
                                        if (value === 'dhcp') {
                                            networkForm.setFieldsValue({
                                                ip: '',
                                                prefixLength: 0,
                                                gateway: '',
                                            });
                                        }
                                    }}
                                />
                            </Form.Item>
                        </div>
                        <Form.Item label="创建网桥" name="bridge" valuePropName="checked">
                            <Switch
                                onChange={(checked) =>
                                    networkForm.setFieldsValue(
                                        checked ? { device: '' } : { bridgePorts: [] }
                                    )
                                }
                            />
                        </Form.Item>
                        {networkBridge ? (
                            <Form.Item label="网桥成员" name="bridgePorts">
                                <Select
                                    mode="multiple"
                                    options={networkDeviceOptions}
                                    placeholder="选择一个或多个物理网卡"
                                    optionFilterProp="label"
                                    showSearch
                                />
                            </Form.Item>
                        ) : (
                            <Form.Item label="物理网卡" name="device">
                                <Select
                                    options={networkDeviceOptions}
                                    placeholder="选择物理网卡"
                                    optionFilterProp="label"
                                    showSearch
                                />
                            </Form.Item>
                        )}
                        {networkMode === 'static' && (
                            <>
                                <div className="grid grid-cols-1 gap-x-4 sm:grid-cols-2">
                                    <Form.Item label="IPv4 地址" name="ip">
                                        <Input placeholder="192.168.1.1" />
                                    </Form.Item>
                                    <Form.Item label="IPv4 前缀长度" name="prefixLength">
                                        <InputNumber className="w-full" min={1} max={30} />
                                    </Form.Item>
                                </div>
                                <Form.Item label="网关（可选）" name="gateway">
                                    <Input placeholder="192.168.1.254" />
                                </Form.Item>
                            </>
                        )}
                    </Form>
                ) : (
                    <>
                        <Flex justify="space-between" align="center" gap={12} className="mb-3">
                            <span className="text-xs text-slate-500">
                                在此集中编辑全部接口；4G 上联网卡已排除，最后一次性原子下发。
                            </span>
                            <Space>
                                <Button
                                    disabled={!networkDraft.some((item) => item.dirty)}
                                    onClick={() =>
                                        setNetworkDraft(
                                            (networkNode?.networks ?? []).map((item) =>
                                                networkDraftFromReported(
                                                    item,
                                                    networkNode?.interfaces ?? []
                                                )
                                            )
                                        )
                                    }
                                >
                                    撤销全部修改
                                </Button>
                                <Button
                                    type="primary"
                                    icon={<PlusOutlined />}
                                    onClick={() => showNetworkEditor()}
                                >
                                    添加接口
                                </Button>
                            </Space>
                        </Flex>
                        <Table
                            rowKey={(item) => item.sourceName ?? item.name}
                            size="small"
                            pagination={false}
                            columns={networkDraftColumns}
                            dataSource={networkDraft}
                            scroll={{ x: 'max-content', y: 360 }}
                        />
                        <div className="mt-4">
                            <div className="mb-1 text-sm">失联自动回滚（秒）</div>
                            <InputNumber
                                min={30}
                                max={300}
                                value={networkRollbackTimeoutSec}
                                onChange={(value) => setNetworkRollbackTimeoutSec(value ?? 60)}
                            />
                            <div className="mt-1 text-xs text-slate-500">
                                节点先保存并应用整套 UCI 配置；若管理链路未恢复，再还原旧配置。
                            </div>
                        </div>
                    </>
                )}
            </FormModal>

            <FormModal
                open={Boolean(renamingNode)}
                zIndex={EDGE_ACTION_MODAL_Z_INDEX}
                title={`修改节点名称${renamingNode ? ` · ${renamingNode.imei}` : ''}`}
                onCancel={() => setRenamingNode(undefined)}
                onOk={() => nameForm.submit()}
                confirmLoading={nodeName.isPending}
                forceRender
                destroyOnHidden
            >
                <Form
                    form={nameForm}
                    layout="vertical"
                    onFinish={(values) => {
                        const parsed = validateForm(nameForm, nodeNameSchema, values);
                        if (parsed && renamingNode)
                            nodeName.mutate(
                                { id: renamingNode.id, data: parsed },
                                { onSuccess: () => setRenamingNode(undefined) }
                            );
                    }}
                >
                    <Form.Item label="节点名称" name="name">
                        <Input maxLength={100} showCount placeholder="请输入节点名称" />
                    </Form.Item>
                </Form>
            </FormModal>

            <FormModal
                open={Boolean(groupingNode)}
                zIndex={EDGE_ACTION_MODAL_Z_INDEX}
                title={`设置节点分组${groupingNode ? ` · ${groupingNode.name || groupingNode.imei}` : ''}`}
                onCancel={() => setGroupingNode(undefined)}
                onOk={() => groupForm.submit()}
                confirmLoading={nodeGroup.isPending}
                forceRender
                destroyOnHidden
            >
                <Form
                    form={groupForm}
                    layout="vertical"
                    onFinish={(values) => {
                        if (!groupingNode) return;
                        nodeGroup.mutate(
                            {
                                id: groupingNode.id,
                                data: { groupId: values.groupId || '' },
                            },
                            { onSuccess: () => setGroupingNode(undefined) }
                        );
                    }}
                >
                    <Form.Item label="所属分组" name="groupId">
                        <TreeSelect
                            allowClear
                            treeDefaultExpandAll
                            treeData={edgeGroupOptions}
                            placeholder="不选则为未分组"
                        />
                    </Form.Item>
                </Form>
            </FormModal>

            <FormModal
                open={firmwareOpen}
                zIndex={EDGE_ACTION_MODAL_Z_INDEX}
                title={`上传固件并刷写${firmwareNode ? ` · ${firmwareNode.name || firmwareNode.imei}` : ''}`}
                onCancel={() => {
                    if (firmwareUpgrade.isPending) return;
                    setFirmwareOpen(false);
                    setFirmwareNode(undefined);
                    setFirmwareUploadProgress(undefined);
                    firmwareUpgrade.reset();
                }}
                onOk={() => firmwareForm.submit()}
                okButtonProps={{ danger: true, disabled: !firmwareFile }}
                cancelButtonProps={{ disabled: firmwareUpgrade.isPending }}
                closable={!firmwareUpgrade.isPending}
                keyboard={!firmwareUpgrade.isPending}
                maskClosable={!firmwareUpgrade.isPending}
                confirmLoading={firmwareUpgrade.isPending}
                destroyOnHidden
            >
                <Form
                    form={firmwareForm}
                    layout="vertical"
                    onFinish={(values) => {
                        const parsed = validateForm(firmwareForm, firmwareUpgradeSchema, values);
                        if (parsed && firmwareNode) {
                            setFirmwareUploadProgress({
                                loadedBytes: 0,
                                totalBytes: parsed.file.size,
                                percent: 0,
                            });
                            firmwareUpgrade.mutate(
                                {
                                    id: firmwareNode.id,
                                    data: parsed,
                                    onProgress: setFirmwareUploadProgress,
                                },
                                {
                                    onSuccess: () => {
                                        setFirmwareOpen(false);
                                        setFirmwareNode(undefined);
                                        setFirmwareUploadProgress(undefined);
                                    },
                                }
                            );
                        }
                    }}
                >
                    <Form.Item
                        label="当前节点固件文件"
                        name="file"
                        getValueFromEvent={(event) => event?.fileList?.[0]?.originFileObj}
                    >
                        <Upload
                            beforeUpload={() => false}
                            maxCount={1}
                            accept=".bin,.img"
                            disabled={firmwareUpgrade.isPending}
                        >
                            <Button disabled={firmwareUpgrade.isPending}>选择固件</Button>
                        </Upload>
                    </Form.Item>
                    <p className="text-xs text-slate-500">
                        最大 128 MiB。上传完成后平台计算 SHA-256，并立即只向当前节点下发；
                        刷写重启后，固件版本以节点实际报告为准。
                    </p>
                    {firmwareUploadProgress && (
                        <div className="mb-5 rounded-lg border border-slate-200 bg-slate-50 px-4 py-3">
                            <Flex justify="space-between" gap={12} className="mb-2 text-xs">
                                <span className="font-medium text-slate-700">
                                    {firmwareUpgrade.isError
                                        ? '上传失败，可直接重试'
                                        : firmwareUploadProgress.percent >= 100
                                          ? '上传完成，平台正在校验并创建刷写任务'
                                          : '正在上传固件到平台'}
                                </span>
                                <span className="shrink-0 text-slate-500">
                                    {formatBytes(firmwareUploadProgress.loadedBytes)} /{' '}
                                    {formatBytes(firmwareUploadProgress.totalBytes)}
                                </span>
                            </Flex>
                            <Progress
                                percent={firmwareUploadProgress.percent}
                                status={firmwareUpgrade.isError ? 'exception' : 'active'}
                                size="small"
                            />
                        </div>
                    )}
                    <Form.Item label="保留 UCI 配置" name="keepSettings" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                    <p className="text-xs text-red-500">
                        节点将下载固件、校验大小和 SHA-256，然后调用
                        sysupgrade；请确认固件与目标硬件完全匹配。
                    </p>
                </Form>
            </FormModal>
            {serialDebug && (
                <SerialDebugModal {...serialDebug} onClose={() => setSerialDebug(undefined)} />
            )}
            <TerminalModal
                nodeId={terminalNode?.id}
                open={terminalOpen}
                onClose={() => {
                    setTerminalOpen(false);
                    setTerminalNode(undefined);
                }}
            />
        </PageContainer>
    );
}

export default EdgeNodePage;
