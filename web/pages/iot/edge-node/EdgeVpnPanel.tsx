import {
    CloudServerOutlined,
    PlusOutlined,
    ReloadOutlined,
    SyncOutlined,
} from '@ant-design/icons';
import {
    Alert,
    Button,
    Descriptions,
    Empty,
    Flex,
    Form,
    Input,
    Modal,
    Popconfirm,
    Select,
    Skeleton,
    Space,
    Switch,
    Table,
    Tag,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { useEffect, useMemo, useState } from 'react';
import { usePermissions } from '@/hooks/usePermission';
import type { Edge } from './edge-node.types';
import {
    useEdgeVpn,
    useEdgeVpnPeerCreate,
    useEdgeVpnPeerRevoke,
    useEdgeVpnPeerSync,
    useEdgeVpnRouteCreate,
    useEdgeVpnRouteDelete,
    useEdgeVpnRouteUpdate,
    useVpnNetworkCreate,
} from './edge-node.vpn.service';
import type { EdgeVpn } from './edge-node.vpn.types';

const statusLabels: Record<string, { color: string; text: string }> = {
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

type RouteFormValues = Omit<EdgeVpn.RouteDto, 'networkId' | 'edgePeerId'>;

export default function EdgeVpnPanel({ node }: { node: Edge.Node }) {
    const { has } = usePermissions();
    const canQuery = has('iot:vpn:query');
    const canAdd = has('iot:vpn:add');
    const canEdit = has('iot:vpn:edit');
    const canDelete = has('iot:vpn:delete');
    const canRevoke = has('iot:vpn:revoke');
    const dataQuery = useEdgeVpn(node.id);
    const data = dataQuery.data;
    const peer = data?.peers.find(
        (item) => item.peerType === 'edge' && item.status !== 'revoked'
    );
    const network = data?.networks.find((item) => item.id === peer?.networkId);
    const [peerOpen, setPeerOpen] = useState(false);
    const [networkOpen, setNetworkOpen] = useState(false);
    const [routeOpen, setRouteOpen] = useState(false);
    const [editingRoute, setEditingRoute] = useState<EdgeVpn.Route | undefined>();
    const [peerForm] = Form.useForm<EdgeVpn.PeerCreateDto>();
    const [networkForm] = Form.useForm<EdgeVpn.NetworkCreateDto>();
    const [routeForm] = Form.useForm<RouteFormValues>();
    const peerCreate = useEdgeVpnPeerCreate();
    const peerSync = useEdgeVpnPeerSync();
    const peerRevoke = useEdgeVpnPeerRevoke();
    const networkCreate = useVpnNetworkCreate();
    const routeCreate = useEdgeVpnRouteCreate();
    const routeUpdate = useEdgeVpnRouteUpdate();
    const routeDelete = useEdgeVpnRouteDelete();

    useEffect(() => {
        if (!peerOpen) return;
        peerForm.setFieldsValue({
            peerType: 'edge',
            networkId: data?.networks[0]?.id,
            edgeNodeId: node.id,
            name: peerName(node),
        });
    }, [data?.networks, node, peerForm, peerOpen]);

    useEffect(() => {
        if (!routeOpen) return;
        routeForm.setFieldsValue(
            editingRoute
                ? {
                      lanInterface: editingRoute.lanInterface,
                      targetCidr: editingRoute.targetCidr,
                      virtualCidr: editingRoute.virtualCidr,
                      mode: editingRoute.mode,
                      enabled: editingRoute.enabled,
                  }
                : {
                      lanInterface: node.networks?.[0]?.name ?? '',
                      targetCidr: '192.168.1.0/24',
                      virtualCidr: '172.31.1.0/24',
                      mode: 'nat',
                      enabled: true,
                  }
        );
    }, [editingRoute, node.networks, routeForm, routeOpen]);

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
                        {canDelete && (
                            <Popconfirm
                                title="确认删除这条 VPN 路由吗？"
                                onConfirm={() => routeDelete.mutate(item.id)}
                            >
                                <Button type="link" size="small" danger>
                                    删除
                                </Button>
                            </Popconfirm>
                        )}
                    </Space>
                ),
            },
        ],
        [canDelete, canEdit, routeDelete]
    );

    if (!canQuery) {
        return <Alert type="warning" showIcon message="您没有 VPN 查询权限" />;
    }

    const supportsVpn = node.capability.vpn?.supportsVpn === true;
    const closePeer = () => {
        if (!peerCreate.isPending) setPeerOpen(false);
    };
    const closeNetwork = () => {
        if (!networkCreate.isPending) setNetworkOpen(false);
    };
    const closeRoute = () => {
        if (!routeCreate.isPending && !routeUpdate.isPending) {
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
            <Flex justify="space-between" align="center" gap={12} wrap>
                <div>
                    <div className="font-medium text-slate-800">节点 VPN</div>
                    <div className="mt-1 text-xs text-slate-500">
                        配置从当前边缘节点进入，平台只保存公钥和路由，不保存节点私钥。
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
                            disabled={!supportsVpn || !data?.networks.length}
                            onClick={() => setPeerOpen(true)}
                        >
                            加入 VPN 网络
                        </Button>
                    )}
                    {canAdd && (
                        <Button icon={<PlusOutlined />} onClick={() => setNetworkOpen(true)}>
                            新建 VPN 网络
                        </Button>
                    )}
                </Space>
            </Flex>

            {dataQuery.isLoading ? (
                <Skeleton active paragraph={{ rows: 4 }} />
            ) : !data?.networks.length ? (
                <Empty description="暂无可用 VPN 网络，请先新建网络" />
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
                        {peer && canAdd && (
                            <Button
                                type="primary"
                                ghost
                                icon={<PlusOutlined />}
                                disabled={!supportsVpn}
                                onClick={() => {
                                    setEditingRoute(undefined);
                                    setRouteOpen(true);
                                }}
                            >
                                添加路由
                            </Button>
                        )}
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

            <Modal
                open={peerOpen}
                title="将边缘节点加入 VPN 网络"
                onCancel={closePeer}
                onOk={() => peerForm.submit()}
                confirmLoading={peerCreate.isPending}
                destroyOnHidden
            >
                <Form
                    form={peerForm}
                    layout="vertical"
                    onFinish={(values) =>
                        peerCreate.mutate(
                            { ...values, peerType: 'edge', edgeNodeId: node.id },
                            { onSuccess: () => setPeerOpen(false) }
                        )
                    }
                >
                    <Form.Item name="networkId" label="VPN 网络" rules={[{ required: true }]}>
                        <Select
                            options={data?.networks.map((item) => ({
                                value: item.id,
                                label: `${item.name} (${item.overlayCidr})`,
                            }))}
                            placeholder="选择 VPN 网络"
                        />
                    </Form.Item>
                    <Form.Item name="name" label="Peer 名称" rules={[{ required: true }]}>
                        <Input maxLength={100} />
                    </Form.Item>
                    <p className="text-xs text-slate-500">
                        Edge 节点私钥只在节点本地生成；节点首次上报公钥后，平台会自动激活 Peer。
                    </p>
                </Form>
            </Modal>

            <Modal
                open={networkOpen}
                title="新建 VPN 网络"
                onCancel={closeNetwork}
                onOk={() => networkForm.submit()}
                confirmLoading={networkCreate.isPending}
                destroyOnHidden
            >
                <Form
                    form={networkForm}
                    layout="vertical"
                    initialValues={{ overlayCidr: '100.96.0.0/24' }}
                    onFinish={(values) =>
                        networkCreate.mutate(values, { onSuccess: () => setNetworkOpen(false) })
                    }
                >
                    <Form.Item name="name" label="网络名称" rules={[{ required: true }]}>
                        <Input maxLength={100} placeholder="例如：生产现场 VPN" />
                    </Form.Item>
                    <Form.Item
                        name="overlayCidr"
                        label="Overlay CIDR"
                        rules={[{ required: true, message: '请输入 Overlay CIDR' }]}
                    >
                        <Input placeholder="100.96.0.0/24" />
                    </Form.Item>
                    <Form.Item name="hubEndpoint" label="Hub 公网地址">
                        <Input placeholder="vpn.example.com" />
                    </Form.Item>
                </Form>
            </Modal>

            <Modal
                open={routeOpen}
                title={editingRoute ? '编辑 VPN 路由' : '添加 VPN 路由'}
                onCancel={closeRoute}
                onOk={() => routeForm.submit()}
                confirmLoading={routeCreate.isPending || routeUpdate.isPending}
                destroyOnHidden
            >
                <Form
                    form={routeForm}
                    layout="vertical"
                    onFinish={(values) => {
                        if (!peer || !network) return;
                        if (editingRoute) {
                            routeUpdate.mutate(
                                { id: editingRoute.id, data: values },
                                { onSuccess: closeRoute }
                            );
                        } else {
                            routeCreate.mutate(
                                { ...values, networkId: network.id, edgePeerId: peer.id },
                                { onSuccess: closeRoute }
                            );
                        }
                    }}
                >
                    <Form.Item
                        name="lanInterface"
                        label="节点 LAN 接口"
                        rules={[{ required: true, message: '请输入 LAN 接口' }]}
                    >
                        <Input placeholder="例如：br-lan" />
                    </Form.Item>
                    <Form.Item
                        name="targetCidr"
                        label="真实 LAN CIDR"
                        rules={[{ required: true, message: '请输入真实 LAN CIDR' }]}
                    >
                        <Input placeholder="192.168.1.0/24" />
                    </Form.Item>
                    <Form.Item
                        name="virtualCidr"
                        label="虚拟 LAN CIDR"
                        rules={[{ required: true, message: '请输入虚拟 LAN CIDR' }]}
                    >
                        <Input placeholder="172.31.1.0/24" />
                    </Form.Item>
                    <Form.Item name="mode" label="模式" rules={[{ required: true }]}>
                        <Select
                            options={[
                                { value: 'nat', label: 'NAT（推荐）' },
                                { value: 'routed', label: '路由' },
                            ]}
                        />
                    </Form.Item>
                    <Form.Item name="enabled" label="启用" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                </Form>
            </Modal>
        </div>
    );
}
