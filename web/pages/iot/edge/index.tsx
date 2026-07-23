import {
    ApiOutlined,
    CheckOutlined,
    CloseOutlined,
    CodeOutlined,
    EditOutlined,
    EyeOutlined,
    GlobalOutlined,
    PlusOutlined,
    SyncOutlined,
    UploadOutlined,
} from '@ant-design/icons';
import { FitAddon } from '@xterm/addon-fit';
import { Terminal } from '@xterm/xterm';
import {
    App,
    Button,
    Descriptions,
    Drawer,
    Empty,
    Flex,
    Form,
    Input,
    InputNumber,
    Modal,
    Pagination,
    Popconfirm,
    Result,
    Select,
    Skeleton,
    Space,
    Switch,
    Table,
    Tabs,
    Tag,
    Tooltip,
    Upload,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import '@xterm/xterm/css/xterm.css';
import { useEffect, useRef, useState } from 'react';
import DeviceCard, { type DeviceCardItem } from '@/components/DeviceCard';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { formatDateTime } from '@/utils/dateTime';
import { validateForm } from '@/utils/validation';
import { getEdgeDetail, getTerminalTicket } from './edge.api';
import {
    firmwareUpgradeSchema,
    networkEditorSchema,
    nodeNameSchema,
    platformSchema,
} from './edge.schema';
import {
    useDeviceConfigSyncMutation,
    useEdgeDetail,
    useEdgeList,
    useEnrollmentMutation,
    useFirmwareUpgradeMutation,
    useNetworkMutation,
    useNodeNameMutation,
    usePlatformDeleteMutation,
    usePlatformMutation,
} from './edge.service';
import type { Edge } from './edge.types';

type NetworkFormValues = Edge.NetworkConfig & { rollbackTimeoutSec: number };

const BOOTSTRAP_URL = 'https://i.a-z.xin';
const EDGE_CARD_GRID_CLASS = 'grid grid-cols-1 gap-3 xl:grid-cols-2 2xl:grid-cols-4';
const EDGE_CARD_ACTION_BUTTON_CLASS =
    '!flex !h-8 !w-8 items-center justify-center !rounded-md text-slate-500 hover:!bg-slate-100 hover:!text-slate-900';
const EDGE_CARD_DANGER_BUTTON_CLASS =
    '!flex !h-8 !w-8 items-center justify-center !rounded-md hover:!bg-red-50';

function statusTag(status: string) {
    const map: Record<string, { color: string; text: string }> = {
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

function formatBytes(value: number) {
    if (value < 1024) return `${value} B`;
    if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
    return `${(value / 1024 / 1024).toFixed(1)} MiB`;
}

function buildNodeCardItems(node: Edge.Node): DeviceCardItem[] {
    return [
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
            children: `${node.activeConfigVersion ?? 0} / ${node.desiredConfigVersion ?? 0}`,
        },
        {
            key: 'outbox',
            label: '待传缓存',
            children: `${node.outboxRecords ?? 0} 条 / ${formatBytes(node.outboxBytes ?? 0)}`,
        },
    ];
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
    const socketRef = useRef<WebSocket | null>(null);
    const terminalHostRef = useRef<HTMLDivElement | null>(null);

    useEffect(() => {
        const host = terminalHostRef.current;
        if (!open || !nodeId || !host) return;

        let disposed = false;
        let poll: number | undefined;
        let fitFrame: number | undefined;
        let lastSentSize = '';
        const terminal = new Terminal({
            cursorBlink: true,
            fontFamily: "'Cascadia Mono', 'SFMono-Regular', Consolas, 'Liberation Mono', monospace",
            fontSize: 14,
            lineHeight: 1.2,
            scrollback: 5_000,
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

        const fitTerminal = () => {
            if (host.clientWidth === 0 || host.clientHeight === 0) return;
            fitAddon.fit();
            const socket = socketRef.current;
            const size = `resize:${terminal.cols}:${terminal.rows}`;
            if (socket?.readyState === WebSocket.OPEN && size !== lastSentSize) {
                socket.send(size);
                lastSentSize = size;
            }
        };
        const resizeObserver = new ResizeObserver(fitTerminal);
        resizeObserver.observe(host);
        fitFrame = window.requestAnimationFrame(fitTerminal);
        const input = terminal.onData((data) => {
            const socket = socketRef.current;
            if (socket?.readyState === WebSocket.OPEN) {
                socket.send(new TextEncoder().encode(data));
            }
        });

        setState('正在申请终端票据…');
        getTerminalTicket(nodeId)
            .then(({ ticket }) => {
                if (disposed) return;
                const transport = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
                const socket = new WebSocket(
                    `${transport}//${window.location.host}/edge/v1/terminal?ticket=${encodeURIComponent(ticket)}`
                );
                socket.binaryType = 'arraybuffer';
                socketRef.current = socket;
                socket.onopen = () => {
                    setState('已连接');
                    fitTerminal();
                    poll = window.setInterval(() => {
                        if (socket.readyState === WebSocket.OPEN) socket.send(new Uint8Array(0));
                    }, 100);
                    terminal.focus();
                };
                socket.onmessage = (event) => {
                    if (typeof event.data === 'string') {
                        if (event.data !== 'ready') setState(event.data || '终端已关闭');
                        return;
                    }
                    terminal.write(new Uint8Array(event.data as ArrayBuffer));
                };
                socket.onerror = () => setState('终端连接失败');
                socket.onclose = () => setState('终端已关闭');
            })
            .catch(() => setState('无法建立终端连接'));
        return () => {
            disposed = true;
            if (poll !== undefined) window.clearInterval(poll);
            if (fitFrame !== undefined) window.cancelAnimationFrame(fitFrame);
            resizeObserver.disconnect();
            input.dispose();
            socketRef.current?.close();
            socketRef.current = null;
            terminal.dispose();
        };
    }, [nodeId, open]);

    return (
        <Modal
            open={open}
            onCancel={onClose}
            footer={null}
            width="min(960px, 94vw)"
            title={`Web 终端 · ${state}`}
            forceRender
            destroyOnHidden
        >
            <div
                ref={terminalHostRef}
                className="h-[min(620px,72dvh)] w-full overflow-hidden rounded-md bg-slate-950 p-3"
                role="application"
                aria-label="边缘节点终端"
            />
        </Modal>
    );
}

export default function EdgeNodePage() {
    const { has } = usePermissions();
    const canQuery = has('iot:edge:query');
    const canEdit = has('iot:edge:edit');
    const canConfig = has('iot:edge:config');
    const canFirmware = has('iot:edge:firmware');
    const canTerminal = has('iot:edge:terminal');
    const [pagination, setPagination] = useState({ page: 1, pageSize: 20 });
    const [keyword, setKeyword] = useState('');
    const [status, setStatus] = useState<Edge.EnrollmentStatus>();
    const [selectedId, setSelectedId] = useState<string>();
    const [renamingNode, setRenamingNode] = useState<Edge.Node>();
    const [networkNode, setNetworkNode] = useState<Edge.Node>();
    const [editingNetwork, setEditingNetwork] = useState<Edge.Network>();
    const [platformNode, setPlatformNode] = useState<Edge.Node>();
    const [firmwareNode, setFirmwareNode] = useState<Edge.Node>();
    const [terminalNode, setTerminalNode] = useState<Edge.Node>();
    const [networkOpen, setNetworkOpen] = useState(false);
    const [platformOpen, setPlatformOpen] = useState(false);
    const [firmwareOpen, setFirmwareOpen] = useState(false);
    const [terminalOpen, setTerminalOpen] = useState(false);
    const [networkForm] = Form.useForm<NetworkFormValues>();
    const [platformForm] = Form.useForm<Edge.PlatformDto>();
    const [firmwareForm] = Form.useForm<Edge.FirmwareUpgradeDto>();
    const [nameForm] = Form.useForm<Edge.NameDto>();
    const networkMode = Form.useWatch('mode', networkForm);
    const networkBridge = Form.useWatch('bridge', networkForm);
    const { modal } = App.useApp();
    const { run: search } = useDebounceFn((value: string) => {
        setKeyword(value);
        setPagination((current) => ({ ...current, page: 1 }));
    }, 300);
    const query = { ...pagination, keyword: keyword || undefined, status };
    const { data, isLoading } = useEdgeList(query, canQuery);
    const nodes: Edge.Node[] = data?.list ?? [];
    const { data: detail, isLoading: detailLoading } = useEdgeDetail(selectedId);
    const enrollment = useEnrollmentMutation();
    const nodeName = useNodeNameMutation();
    const network = useNetworkMutation();
    const deviceConfigSync = useDeviceConfigSyncMutation();
    const platform = usePlatformMutation();
    const platformDelete = usePlatformDeleteMutation();
    const firmwareUpgrade = useFirmwareUpgradeMutation();

    useEffect(() => {
        if (!selectedId) return;
        const exists = data?.list.some((node) => node.id === selectedId) ?? true;
        if (!exists) setSelectedId(undefined);
    }, [data, selectedId]);

    if (!canQuery) {
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询边缘节点的权限" />
            </PageContainer>
        );
    }

    const updateEnrollment = (node: Edge.Node, next: 'approved' | 'rejected') => {
        modal.confirm({
            title:
                next === 'approved' ? `批准 IMEI ${node.imei} 注册？` : `拒绝 IMEI ${node.imei}？`,
            content:
                next === 'approved'
                    ? '批准后节点下次连接即可进入在线状态。'
                    : '拒绝后节点将无法建立管理会话。',
            okButtonProps: { danger: next === 'rejected' },
            onOk: () =>
                enrollment.mutateAsync({
                    id: node.id,
                    status: next,
                    name: node.name || node.hostname,
                }),
        });
    };

    const showNetworkManager = async (node: Edge.Node) => {
        setNetworkNode(node.networks && node.interfaces ? node : await getEdgeDetail(node.id));
    };

    const refreshNetworkManager = async () => {
        if (!networkNode) return;
        setNetworkNode(await getEdgeDetail(networkNode.id));
    };

    const showNetworkEditor = (item?: Edge.Network) => {
        const firstDevice = networkNode?.interfaces?.find((candidate) => !candidate.bridge)?.name;
        networkForm.setFieldsValue(
            item
                ? {
                      operation: 'upsert',
                      name: item.name,
                      mode: item.mode === 'static' ? 'static' : 'dhcp',
                      device: item.bridge ? '' : item.device,
                      bridge: item.bridge,
                      bridgePorts: item.bridgePorts,
                      ip: item.mode === 'static' ? item.ipv4 : '',
                      prefixLength: item.mode === 'static' ? item.prefixLength : 0,
                      gateway: item.mode === 'static' ? item.gateway : '',
                      rollbackTimeoutSec: 60,
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
                      rollbackTimeoutSec: 60,
                  }
        );
        setEditingNetwork(item);
        setNetworkOpen(true);
    };

    const showRename = (node: Edge.Node) => {
        nameForm.setFieldsValue({ name: node.name || node.hostname });
        setRenamingNode(node);
    };

    const showPlatform = (item?: Edge.Platform) => {
        platformForm.setFieldsValue(
            item
                ? { ...item, enrollmentToken: '' }
                : {
                      name: '',
                      baseUrl: 'https://',
                      enrollmentToken: '',
                      enabled: true,
                      priority: 100,
                      reconnectIntervalSec: 5,
                      outboxMaxBytes: 262144,
                  }
        );
        setPlatformOpen(true);
    };

    const showPlatformManager = async (node: Edge.Node) => {
        setPlatformNode(node.platforms ? node : await getEdgeDetail(node.id));
    };

    const refreshPlatformManager = async () => {
        if (!platformNode) return;
        setPlatformNode(await getEdgeDetail(platformNode.id));
    };

    const showFirmware = (node: Edge.Node) => {
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
    const networkColumns: ColumnsType<Edge.Network> = [
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
        {
            title: '操作',
            width: 120,
            render: (_, item) => (
                <Space size={4}>
                    <Button type="link" size="small" onClick={() => showNetworkEditor(item)}>
                        编辑
                    </Button>
                    <Popconfirm
                        title={`删除逻辑接口 ${item.name}？`}
                        description="节点应用后若无法重新连接，将在超时后恢复原网络配置。"
                        onConfirm={() => {
                            if (!networkNode) return;
                            network.mutate(
                                {
                                    id: networkNode.id,
                                    data: {
                                        interfaces: [{ operation: 'delete', name: item.name }],
                                        rollbackTimeoutSec: 60,
                                    },
                                },
                                { onSuccess: refreshNetworkManager }
                            );
                        }}
                    >
                        <Button type="link" size="small" danger>
                            删除
                        </Button>
                    </Popconfirm>
                </Space>
            ),
        },
    ];
    const assignedNetworkDevices = new Set(
        (networkNode?.networks ?? [])
            .filter((item) => item.name !== editingNetwork?.name)
            .flatMap((item) => (item.bridge ? item.bridgePorts : item.device ? [item.device] : []))
    );
    const networkDeviceOptions = (networkNode?.interfaces ?? [])
        .filter((item) => !item.bridge)
        .map((item) => ({
            value: item.name,
            label: item.displayName ? `${item.displayName} (${item.name})` : item.name,
            disabled: assignedNetworkDevices.has(item.name),
        }));
    const serialColumns: ColumnsType<Edge.SerialPort> = [
        { title: '串口', dataIndex: 'path' },
        { title: '名称', dataIndex: 'displayName' },
        { title: '可读写', dataIndex: 'available', render: (value) => (value ? '是' : '否') },
        { title: 'RS485', dataIndex: 'rs485', render: (value) => (value ? '是' : '未确认') },
    ];
    const platformInfoColumns: ColumnsType<Edge.Platform> = [
        { title: '名称', dataIndex: 'name' },
        { title: 'HTTP(S) 地址', dataIndex: 'baseUrl' },
        { title: '状态', dataIndex: 'applyStatus', render: statusTag },
        { title: '启用', dataIndex: 'enabled', render: (value) => (value ? '是' : '否') },
    ];
    const platformColumns: ColumnsType<Edge.Platform> = [
        ...platformInfoColumns,
        {
            title: '操作',
            width: 130,
            render: (_, item) => (
                <Space>
                    <Button type="link" onClick={() => showPlatform(item)}>
                        编辑
                    </Button>
                    <Popconfirm
                        title="删除这个平台配置？"
                        onConfirm={() =>
                            platformNode &&
                            platformDelete.mutate(
                                { id: platformNode.id, platformId: item.platformId },
                                { onSuccess: refreshPlatformManager }
                            )
                        }
                    >
                        <Button type="link" danger>
                            删除
                        </Button>
                    </Popconfirm>
                </Space>
            ),
        },
    ];
    const taskColumns: ColumnsType<Edge.Task> = [
        { title: '类型', dataIndex: 'taskType' },
        { title: '状态', dataIndex: 'status', render: statusTag },
        { title: '结果', dataIndex: 'message', render: (value) => value || '-' },
        {
            title: '创建时间',
            dataIndex: 'createdAt',
            render: (value) => formatDateTime(value),
        },
    ];

    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-center justify-between gap-3">
                    <div>
                        <h2 className="m-0 text-lg font-semibold text-slate-900">边缘节点</h2>
                        <p className="m-0 mt-1 text-xs text-slate-500">
                            固化平台 {BOOTSTRAP_URL}，由此配置节点上报到其他 HTTP(S) 平台
                        </p>
                    </div>
                    <Space wrap>
                        <Input.Search
                            allowClear
                            className="w-[240px]"
                            placeholder="IMEI / 名称 / 型号"
                            onChange={(event) => search(event.target.value)}
                        />
                        <Select
                            allowClear
                            className="w-[130px]"
                            placeholder="注册状态"
                            value={status}
                            onChange={(value) => {
                                setStatus(value);
                                setPagination((current) => ({ ...current, page: 1 }));
                            }}
                            options={[
                                { value: 'pending', label: '待处理' },
                                { value: 'approved', label: '已批准' },
                                { value: 'rejected', label: '已拒绝' },
                            ]}
                        />
                    </Space>
                </div>
            }
            footer={
                <div className="flex justify-end">
                    <Pagination
                        {...pagination}
                        total={data?.total ?? 0}
                        showSizeChanger
                        showTotal={(total) => `共 ${total} 条`}
                        onChange={(page, pageSize) => setPagination({ page, pageSize })}
                    />
                </div>
            }
        >
            <div className="h-full overflow-y-auto overflow-x-hidden">
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
                    <div className={EDGE_CARD_GRID_CLASS}>
                        {nodes.map((node) => (
                            <div key={node.id} className="flex flex-col">
                                <DeviceCard
                                    title={
                                        <Flex
                                            justify="space-between"
                                            align="start"
                                            gap={10}
                                            className="w-full min-w-0"
                                        >
                                            <span className="min-w-0 flex-1 whitespace-normal break-words pr-1 text-left leading-5">
                                                {node.name || node.hostname || '未命名节点'}
                                                <span className="ml-2 whitespace-nowrap text-xs font-normal text-slate-400">
                                                    IMEI：{node.imei}
                                                </span>
                                            </span>
                                            <Tag
                                                color={node.online ? 'success' : 'default'}
                                                className="!mr-0 shrink-0 !rounded-md !px-2"
                                            >
                                                {node.online ? '在线' : '离线'}
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
                                                上报：{formatDateTime(node.lastSeenAt)}
                                            </span>
                                        </div>
                                    }
                                    items={buildNodeCardItems(node)}
                                    column={8}
                                    extra={
                                        <Flex
                                            align="center"
                                            justify="center"
                                            gap={10}
                                            wrap
                                            className="w-full"
                                        >
                                            <Tooltip title="查看详情">
                                                <Button
                                                    type="text"
                                                    size="small"
                                                    className={EDGE_CARD_ACTION_BUTTON_CLASS}
                                                    icon={<EyeOutlined />}
                                                    onClick={() => setSelectedId(node.id)}
                                                />
                                            </Tooltip>
                                            {canEdit && (
                                                <Tooltip title="修改名称">
                                                    <Button
                                                        type="text"
                                                        size="small"
                                                        className={EDGE_CARD_ACTION_BUTTON_CLASS}
                                                        icon={<EditOutlined />}
                                                        onClick={() => showRename(node)}
                                                    />
                                                </Tooltip>
                                            )}
                                            {node.enrollmentStatus === 'approved' &&
                                                canConfig &&
                                                node.supportsDeviceConfig && (
                                                    <Tooltip title="同步设备配置">
                                                        <Button
                                                            type="text"
                                                            size="small"
                                                            className={
                                                                EDGE_CARD_ACTION_BUTTON_CLASS
                                                            }
                                                            icon={<SyncOutlined />}
                                                            loading={
                                                                deviceConfigSync.isPending &&
                                                                deviceConfigSync.variables ===
                                                                    node.id
                                                            }
                                                            onClick={() =>
                                                                deviceConfigSync.mutate(node.id)
                                                            }
                                                        />
                                                    </Tooltip>
                                                )}
                                            {node.enrollmentStatus === 'approved' &&
                                                canConfig &&
                                                node.supportsNetworkConfig &&
                                                node.networkConfigVersion >= 2 && (
                                                    <Tooltip title="管理网络接口">
                                                        <Button
                                                            type="text"
                                                            size="small"
                                                            className={
                                                                EDGE_CARD_ACTION_BUTTON_CLASS
                                                            }
                                                            icon={<GlobalOutlined />}
                                                            onClick={() =>
                                                                void showNetworkManager(node)
                                                            }
                                                        />
                                                    </Tooltip>
                                                )}
                                            {node.enrollmentStatus === 'approved' &&
                                                canConfig &&
                                                node.supportsPlatformConfig && (
                                                    <Tooltip title="管理其他平台">
                                                        <Button
                                                            type="text"
                                                            size="small"
                                                            className={
                                                                EDGE_CARD_ACTION_BUTTON_CLASS
                                                            }
                                                            icon={<ApiOutlined />}
                                                            onClick={() =>
                                                                void showPlatformManager(node)
                                                            }
                                                        />
                                                    </Tooltip>
                                                )}
                                            {node.enrollmentStatus === 'approved' &&
                                                canFirmware &&
                                                node.supportsFirmwareUpdate && (
                                                    <Tooltip title="上传固件并刷写">
                                                        <Button
                                                            type="text"
                                                            danger
                                                            size="small"
                                                            className={
                                                                EDGE_CARD_DANGER_BUTTON_CLASS
                                                            }
                                                            icon={<UploadOutlined />}
                                                            onClick={() => showFirmware(node)}
                                                        />
                                                    </Tooltip>
                                                )}
                                            {node.enrollmentStatus === 'approved' &&
                                                canTerminal &&
                                                node.ttydAvailable && (
                                                    <Tooltip title="Web 终端">
                                                        <Button
                                                            type="text"
                                                            size="small"
                                                            className={
                                                                EDGE_CARD_ACTION_BUTTON_CLASS
                                                            }
                                                            icon={<CodeOutlined />}
                                                            onClick={() => {
                                                                setTerminalNode(node);
                                                                setTerminalOpen(true);
                                                            }}
                                                        />
                                                    </Tooltip>
                                                )}
                                            {canEdit && node.enrollmentStatus !== 'approved' && (
                                                <Tooltip title="批准注册">
                                                    <Button
                                                        type="text"
                                                        size="small"
                                                        className={EDGE_CARD_ACTION_BUTTON_CLASS}
                                                        icon={<CheckOutlined />}
                                                        onClick={() =>
                                                            updateEnrollment(node, 'approved')
                                                        }
                                                    />
                                                </Tooltip>
                                            )}
                                            {canEdit && node.enrollmentStatus !== 'rejected' && (
                                                <Tooltip title="拒绝注册">
                                                    <Button
                                                        type="text"
                                                        danger
                                                        size="small"
                                                        className={EDGE_CARD_DANGER_BUTTON_CLASS}
                                                        icon={<CloseOutlined />}
                                                        onClick={() =>
                                                            updateEnrollment(node, 'rejected')
                                                        }
                                                    />
                                                </Tooltip>
                                            )}
                                        </Flex>
                                    }
                                />
                            </div>
                        ))}
                    </div>
                )}
            </div>

            <Drawer
                open={Boolean(selectedId)}
                onClose={() => setSelectedId(undefined)}
                title={
                    detail
                        ? `${detail.name || detail.hostname || '边缘节点'} · ${detail.imei}`
                        : '边缘节点详情'
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
                                {detail.online ? <Tag color="success">在线</Tag> : <Tag>离线</Tag>}
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
                            <Descriptions.Item label="缓存">
                                {detail.outboxRecords} 条 / {formatBytes(detail.outboxBytes)}
                            </Descriptions.Item>
                            <Descriptions.Item label="设备配置">
                                {statusTag(detail.configStatus)} V{detail.activeConfigVersion} / V
                                {detail.desiredConfigVersion}
                                {detail.configMessage ? ` · ${detail.configMessage}` : ''}
                            </Descriptions.Item>
                            <Descriptions.Item label="ttyd">
                                {detail.ttydAvailable ? (
                                    <Tag color="success">已检测</Tag>
                                ) : (
                                    <Tag>未安装</Tag>
                                )}
                            </Descriptions.Item>
                            <Descriptions.Item label="最近上报">
                                {formatDateTime(detail.lastSeenAt)}
                            </Descriptions.Item>
                        </Descriptions>
                        <Tabs
                            className="mt-4"
                            items={[
                                {
                                    key: 'networks',
                                    label: `逻辑接口 (${detail.networks?.length ?? 0})`,
                                    children: (
                                        <Table
                                            rowKey="name"
                                            size="small"
                                            pagination={false}
                                            columns={networkColumns.slice(0, -1)}
                                            dataSource={detail.networks ?? []}
                                            scroll={{ x: 'max-content', y: 360 }}
                                        />
                                    ),
                                },
                                {
                                    key: 'interfaces',
                                    label: `物理网卡 (${detail.interfaces?.length ?? 0})`,
                                    children: (
                                        <Table
                                            rowKey="name"
                                            size="small"
                                            pagination={false}
                                            columns={interfaceColumns}
                                            dataSource={detail.interfaces ?? []}
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
                                    key: 'platforms',
                                    label: `其他平台 (${detail.platforms?.length ?? 0})`,
                                    children: (
                                        <Table
                                            rowKey="platformId"
                                            size="small"
                                            pagination={false}
                                            columns={platformInfoColumns}
                                            dataSource={detail.platforms ?? []}
                                            scroll={{ x: 'max-content', y: 360 }}
                                        />
                                    ),
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
                                            dataSource={detail.tasks ?? []}
                                            scroll={{ x: 'max-content', y: 360 }}
                                        />
                                    ),
                                },
                            ]}
                        />
                    </>
                )}
            </Drawer>

            <Modal
                open={Boolean(networkNode)}
                onCancel={() => setNetworkNode(undefined)}
                footer={null}
                width="min(900px, 92vw)"
                title={`网络接口${networkNode ? ` · ${networkNode.name || networkNode.imei}` : ''}`}
                destroyOnHidden
            >
                <Flex justify="space-between" align="center" gap={12} className="mb-3">
                    <span className="text-xs text-slate-500">
                        仅显示节点允许管理的网卡，4G 上联网卡不会出现在候选列表中。
                    </span>
                    <Button
                        type="primary"
                        icon={<PlusOutlined />}
                        onClick={() => showNetworkEditor()}
                    >
                        添加接口
                    </Button>
                </Flex>
                <Table
                    rowKey="name"
                    size="small"
                    pagination={false}
                    columns={networkColumns}
                    dataSource={networkNode?.networks ?? []}
                    scroll={{ x: 'max-content', y: 360 }}
                />
            </Modal>

            <Modal
                open={Boolean(platformNode)}
                onCancel={() => setPlatformNode(undefined)}
                footer={null}
                width="min(880px, 92vw)"
                title={`其他平台${platformNode ? ` · ${platformNode.name || platformNode.imei}` : ''}`}
                destroyOnHidden
            >
                <Flex justify="flex-end" className="mb-3">
                    <Button type="primary" icon={<ApiOutlined />} onClick={() => showPlatform()}>
                        添加平台
                    </Button>
                </Flex>
                <Table
                    rowKey="platformId"
                    size="small"
                    pagination={false}
                    columns={platformColumns}
                    dataSource={platformNode?.platforms ?? []}
                    scroll={{ x: 'max-content', y: 360 }}
                />
            </Modal>

            <FormModal
                open={Boolean(renamingNode)}
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
                open={networkOpen}
                title={`${editingNetwork ? '编辑' : '添加'}网络接口${
                    networkNode ? ` · ${networkNode.name || networkNode.imei}` : ''
                }`}
                onCancel={() => {
                    setNetworkOpen(false);
                    setEditingNetwork(undefined);
                }}
                onOk={() => networkForm.submit()}
                confirmLoading={network.isPending}
                forceRender
                destroyOnHidden
            >
                <Form
                    form={networkForm}
                    layout="vertical"
                    onFinish={(values) => {
                        const parsed = validateForm(networkForm, networkEditorSchema, values);
                        if (parsed && networkNode) {
                            const { rollbackTimeoutSec, ...item } = parsed;
                            network.mutate(
                                {
                                    id: networkNode.id,
                                    data: {
                                        interfaces: [{ ...item, operation: 'upsert' }],
                                        rollbackTimeoutSec,
                                    },
                                },
                                {
                                    onSuccess: async () => {
                                        setNetworkOpen(false);
                                        setEditingNetwork(undefined);
                                        await refreshNetworkManager();
                                    },
                                }
                            );
                        }
                    }}
                >
                    <Form.Item name="operation" hidden>
                        <Input />
                    </Form.Item>
                    <div className="grid grid-cols-1 gap-x-4 sm:grid-cols-2">
                        <Form.Item label="逻辑接口名称" name="name">
                            <Input
                                maxLength={15}
                                disabled={Boolean(editingNetwork)}
                                placeholder="例如 lan"
                            />
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
                    <Form.Item label="失联自动回滚（秒）" name="rollbackTimeoutSec">
                        <InputNumber className="w-full" min={30} max={300} />
                    </Form.Item>
                    <p className="text-xs text-slate-500">
                        节点通过 UCI
                        原子应用配置；新网络无法让节点重新连接平台时，会自动恢复原配置。
                    </p>
                </Form>
            </FormModal>

            <FormModal
                open={platformOpen}
                title={`配置其他平台${platformNode ? ` · ${platformNode.name || platformNode.imei}` : ''}`}
                onCancel={() => setPlatformOpen(false)}
                onOk={() => platformForm.submit()}
                confirmLoading={platform.isPending}
                destroyOnHidden
            >
                <Form
                    form={platformForm}
                    layout="vertical"
                    onFinish={(values) => {
                        const parsed = validateForm(platformForm, platformSchema, values);
                        if (parsed && platformNode)
                            platform.mutate(
                                { id: platformNode.id, data: parsed },
                                {
                                    onSuccess: async () => {
                                        setPlatformOpen(false);
                                        await refreshPlatformManager();
                                    },
                                }
                            );
                    }}
                >
                    <Form.Item name="platformId" hidden>
                        <Input />
                    </Form.Item>
                    <Form.Item label="平台名称" name="name">
                        <Input maxLength={32} />
                    </Form.Item>
                    <Form.Item label="平台 HTTP(S) 地址" name="baseUrl">
                        <Input placeholder="https://example.com" />
                    </Form.Item>
                    <Form.Item label="注册凭据（可选）" name="enrollmentToken">
                        <Input.Password maxLength={192} />
                    </Form.Item>
                    <div className="grid grid-cols-1 gap-x-4 sm:grid-cols-2">
                        <Form.Item label="优先级" name="priority">
                            <InputNumber className="w-full" min={0} max={65535} />
                        </Form.Item>
                        <Form.Item label="重连间隔（秒）" name="reconnectIntervalSec">
                            <InputNumber className="w-full" min={1} max={3600} />
                        </Form.Item>
                        <Form.Item label="tmpfs 上报缓存上限（字节）" name="outboxMaxBytes">
                            <InputNumber className="w-full" min={16384} max={8388608} />
                        </Form.Item>
                        <Form.Item label="启用" name="enabled" valuePropName="checked">
                            <Switch />
                        </Form.Item>
                    </div>
                    <p className="text-xs text-slate-500">
                        平台地址只填写 HTTP 或 HTTPS；节点内部自动建立对应长连接。
                    </p>
                </Form>
            </FormModal>

            <FormModal
                open={firmwareOpen}
                title={`上传固件并刷写${firmwareNode ? ` · ${firmwareNode.name || firmwareNode.imei}` : ''}`}
                onCancel={() => {
                    setFirmwareOpen(false);
                    setFirmwareNode(undefined);
                }}
                onOk={() => firmwareForm.submit()}
                okButtonProps={{ danger: true }}
                confirmLoading={firmwareUpgrade.isPending}
                destroyOnHidden
            >
                <Form
                    form={firmwareForm}
                    layout="vertical"
                    onFinish={(values) => {
                        const parsed = validateForm(firmwareForm, firmwareUpgradeSchema, values);
                        if (parsed && firmwareNode)
                            firmwareUpgrade.mutate(
                                { id: firmwareNode.id, data: parsed },
                                {
                                    onSuccess: () => {
                                        setFirmwareOpen(false);
                                        setFirmwareNode(undefined);
                                    },
                                }
                            );
                    }}
                >
                    <Form.Item label="版本" name="version">
                        <Input maxLength={64} placeholder="例如 23.05.5-r1" />
                    </Form.Item>
                    <Form.Item
                        label="当前节点固件文件"
                        name="file"
                        getValueFromEvent={(event) => event?.fileList?.[0]?.originFileObj}
                    >
                        <Upload beforeUpload={() => false} maxCount={1} accept=".bin,.img">
                            <Button>选择固件</Button>
                        </Upload>
                    </Form.Item>
                    <p className="text-xs text-slate-500">
                        最大 128 MiB。上传完成后平台计算 SHA-256，并立即只向当前节点下发。
                    </p>
                    <Form.Item label="保留 UCI 配置" name="keepSettings" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                    <p className="text-xs text-red-500">
                        节点将下载固件、校验大小和 SHA-256，然后调用
                        sysupgrade；请确认固件与目标硬件完全匹配。
                    </p>
                </Form>
            </FormModal>
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
