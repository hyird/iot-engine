import {
    CloudServerOutlined,
    ReloadOutlined,
    SyncOutlined,
} from '@ant-design/icons';
import {
    Alert,
    Button,
    Descriptions,
    Flex,
    Form,
    Modal,
    Empty,
    Input,
    Popconfirm,
    Skeleton,
    Space,
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
    useEdgeVpnRouteUpdate,
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

type RouteFormValues = Pick<EdgeVpn.RouteDto, 'virtualCidr'>;

const VPN_MODAL_Z_INDEX = 1100;

export default function EdgeVpnPanel({ node }: { node: Edge.Node }) {
    const { has } = usePermissions();
    const canQuery = has('iot:vpn:query');
    const canAdd = has('iot:vpn:add');
    const canEdit = has('iot:vpn:edit');
    const canRevoke = has('iot:vpn:revoke');
    const dataQuery = useEdgeVpn(node.id);
    const data = dataQuery.data;
    const peer = data?.peers.find(
        (item) => item.peerType === 'edge' && item.status !== 'revoked'
    );
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

            <Modal
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
                        真实 LAN、桥接接口、掩码和 NAT 模式由 EdgeNode 自动确定；只允许修改虚拟网段网络号，
                        且必须与真实 LAN 使用相同掩码并保持全局唯一。
                    </p>
                    <Form.Item
                        name="virtualCidr"
                        label="虚拟 LAN 网段"
                        rules={[
                            { required: true, message: '请输入虚拟 LAN 网段' },
                            {
                                pattern: /^172\.31\.\d{1,3}\.\d{1,3}\/\d{1,2}$/,
                                message: '请输入 172.31.0.0/16 范围内的网络 CIDR',
                            },
                        ]}
                    >
                        <Input placeholder="172.31.1.0/24" />
                    </Form.Item>
                </Form>
            </Modal>

        </div>
    );
}
