import {
    App,
    Button,
    Descriptions,
    Drawer,
    Form,
    Input,
    InputNumber,
    Modal,
    Pagination,
    Popconfirm,
    Result,
    Select,
    Space,
    Switch,
    Table,
    Tabs,
    Tag,
    Upload,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { FitAddon } from '@xterm/addon-fit';
import { Terminal } from '@xterm/xterm';
import '@xterm/xterm/css/xterm.css';
import { useEffect, useRef, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { validateForm } from '@/utils/validation';
import { getTerminalTicket } from './edge.api';
import {
    firmwareUpgradeSchema,
    networkSchema,
    nodeNameSchema,
    platformSchema,
} from './edge.schema';
import {
    useEdgeDetail,
    useEdgeList,
    useDeviceConfigSyncMutation,
    useEnrollmentMutation,
    useFirmwareUpgradeMutation,
    useNodeNameMutation,
    useNetworkMutation,
    usePlatformDeleteMutation,
    usePlatformMutation,
} from './edge.service';
import type { Edge } from './edge.types';

const BOOTSTRAP_URL = 'https://i.a-z.xin';

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
    const [networkOpen, setNetworkOpen] = useState(false);
    const [platformOpen, setPlatformOpen] = useState(false);
    const [firmwareOpen, setFirmwareOpen] = useState(false);
    const [terminalOpen, setTerminalOpen] = useState(false);
    const [networkForm] = Form.useForm<Edge.NetworkDto>();
    const [platformForm] = Form.useForm<Edge.PlatformDto>();
    const [firmwareForm] = Form.useForm<Edge.FirmwareUpgradeDto>();
    const [nameForm] = Form.useForm<Edge.NameDto>();
    const { modal } = App.useApp();
    const { run: search } = useDebounceFn((value: string) => {
        setKeyword(value);
        setPagination((current) => ({ ...current, page: 1 }));
    }, 300);
    const query = { ...pagination, keyword: keyword || undefined, status };
    const { data, isLoading } = useEdgeList(query, canQuery);
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

    const showNetwork = () => {
        const current = detail?.interfaces?.find((item) => item.name === 'br-lan');
        const prefix = current?.prefixLength ?? 24;
        const mask = prefix > 0 && prefix <= 32 ? (0xffffffff << (32 - prefix)) >>> 0 : 0xffffff00;
        networkForm.setFieldsValue({
            ip: current?.ipv4 || '',
            netmask: [24, 16, 8, 0].map((shift) => (mask >>> shift) & 255).join('.'),
            gateway: current?.gateway || '',
            rollbackTimeoutSec: 60,
        });
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

    const columns: ColumnsType<Edge.Node> = [
        { title: 'IMEI', dataIndex: 'imei', width: 160 },
        {
            title: '节点名称',
            dataIndex: 'name',
            render: (value, node) => value || node.hostname || '-',
        },
        { title: '型号', dataIndex: 'model', render: (value) => value || '-' },
        {
            title: '连接',
            dataIndex: 'online',
            width: 90,
            render: (online) => (
                <Tag color={online ? 'success' : 'default'}>{online ? '在线' : '离线'}</Tag>
            ),
        },
        {
            title: '注册状态',
            dataIndex: 'enrollmentStatus',
            width: 110,
            render: statusTag,
        },
        { title: '版本', dataIndex: 'softwareVersion', render: (value) => value || '-' },
        { title: '最近上报', dataIndex: 'lastSeenAt', render: (value) => value || '-' },
        {
            title: '操作',
            key: 'actions',
            fixed: 'right',
            width: 250,
            render: (_, node) => (
                <Space>
                    <Button type="link" onClick={() => setSelectedId(node.id)}>
                        详情
                    </Button>
                    {canEdit && (
                        <Button type="link" onClick={() => showRename(node)}>
                            改名
                        </Button>
                    )}
                    {canEdit && node.enrollmentStatus !== 'approved' && (
                        <Button type="link" onClick={() => updateEnrollment(node, 'approved')}>
                            批准
                        </Button>
                    )}
                    {canEdit && node.enrollmentStatus !== 'rejected' && (
                        <Button
                            type="link"
                            danger
                            onClick={() => updateEnrollment(node, 'rejected')}
                        >
                            拒绝
                        </Button>
                    )}
                </Space>
            ),
        },
    ];

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
    const serialColumns: ColumnsType<Edge.SerialPort> = [
        { title: '串口', dataIndex: 'path' },
        { title: '名称', dataIndex: 'displayName' },
        { title: '可读写', dataIndex: 'available', render: (value) => (value ? '是' : '否') },
        { title: 'RS485', dataIndex: 'rs485', render: (value) => (value ? '是' : '未确认') },
    ];
    const platformColumns: ColumnsType<Edge.Platform> = [
        { title: '名称', dataIndex: 'name' },
        { title: 'HTTP(S) 地址', dataIndex: 'baseUrl' },
        { title: '状态', dataIndex: 'applyStatus', render: statusTag },
        { title: '启用', dataIndex: 'enabled', render: (value) => (value ? '是' : '否') },
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
                            selectedId &&
                            platformDelete.mutate({ id: selectedId, platformId: item.platformId })
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
        { title: '创建时间', dataIndex: 'createdAt' },
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
            <Table
                rowKey="id"
                columns={columns}
                dataSource={data?.list ?? []}
                loading={isLoading}
                pagination={false}
                sticky
                scroll={{ x: 'max-content', y: 'calc(100vh - 280px)' }}
            />

            <Drawer
                open={Boolean(selectedId)}
                onClose={() => setSelectedId(undefined)}
                title={
                    detail
                        ? `${detail.name || detail.hostname || '边缘节点'} · ${detail.imei}`
                        : '边缘节点详情'
                }
                width="min(960px, 92vw)"
                loading={detailLoading}
                extra={
                    detail ? (
                        <Space wrap>
                            {canEdit && (
                                <Button onClick={() => showRename(detail)}>修改名称</Button>
                            )}
                            {detail.enrollmentStatus === 'approved' &&
                                canConfig &&
                                detail.supportsDeviceConfig && (
                                    <Button
                                        type="primary"
                                        loading={deviceConfigSync.isPending}
                                        onClick={() => deviceConfigSync.mutate(detail.id)}
                                    >
                                        同步设备配置
                                    </Button>
                                )}
                            {detail.enrollmentStatus === 'approved' &&
                                canConfig &&
                                detail.supportsNetworkConfig && (
                                    <Button onClick={showNetwork}>配置 br-lan</Button>
                                )}
                            {detail.enrollmentStatus === 'approved' &&
                                canConfig &&
                                detail.supportsPlatformConfig && (
                                    <Button onClick={() => showPlatform()}>添加平台</Button>
                                )}
                            {detail.enrollmentStatus === 'approved' &&
                                canFirmware &&
                                detail.supportsFirmwareUpdate && (
                                    <Button
                                        danger
                                        onClick={() => {
                                            firmwareForm.resetFields();
                                            firmwareForm.setFieldsValue({ keepSettings: true });
                                            setFirmwareOpen(true);
                                        }}
                                    >
                                        上传固件并刷写
                                    </Button>
                                )}
                            {detail.enrollmentStatus === 'approved' &&
                                canTerminal &&
                                detail.ttydAvailable && (
                                    <Button onClick={() => setTerminalOpen(true)}>Web 终端</Button>
                                )}
                        </Space>
                    ) : null
                }
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
                                {detail.lastSeenAt || '-'}
                            </Descriptions.Item>
                        </Descriptions>
                        <Tabs
                            className="mt-4"
                            items={[
                                {
                                    key: 'network',
                                    label: `网口 (${detail.interfaces?.length ?? 0})`,
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
                                            columns={platformColumns}
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
                title="配置 br-lan"
                onCancel={() => setNetworkOpen(false)}
                onOk={() => networkForm.submit()}
                confirmLoading={network.isPending}
                destroyOnHidden
            >
                <Form
                    form={networkForm}
                    layout="vertical"
                    onFinish={(values) => {
                        const parsed = validateForm(networkForm, networkSchema, values);
                        if (parsed && selectedId)
                            network.mutate(
                                { id: selectedId, data: parsed },
                                { onSuccess: () => setNetworkOpen(false) }
                            );
                    }}
                >
                    <Form.Item label="接口">
                        <Input value="br-lan" disabled />
                    </Form.Item>
                    <div className="grid grid-cols-1 gap-x-4 sm:grid-cols-2">
                        <Form.Item label="IPv4 地址" name="ip">
                            <Input placeholder="192.168.1.1" />
                        </Form.Item>
                        <Form.Item label="子网掩码" name="netmask">
                            <Input placeholder="255.255.255.0" />
                        </Form.Item>
                    </div>
                    <Form.Item label="网关（可选）" name="gateway">
                        <Input placeholder="192.168.1.254" />
                    </Form.Item>
                    <Form.Item label="失联自动回滚（秒）" name="rollbackTimeoutSec">
                        <InputNumber className="w-full" min={30} max={300} />
                    </Form.Item>
                    <p className="text-xs text-slate-500">
                        节点只通过 UCI 命令修改网络；新地址无法重新连接时会自动恢复原配置。
                    </p>
                </Form>
            </FormModal>

            <FormModal
                open={platformOpen}
                title="配置其他平台"
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
                        if (parsed && selectedId)
                            platform.mutate(
                                { id: selectedId, data: parsed },
                                { onSuccess: () => setPlatformOpen(false) }
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
                title={`上传固件并刷写${detail ? ` · ${detail.name || detail.imei}` : ''}`}
                onCancel={() => setFirmwareOpen(false)}
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
                        if (parsed && selectedId)
                            firmwareUpgrade.mutate(
                                { id: selectedId, data: parsed },
                                { onSuccess: () => setFirmwareOpen(false) }
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
                nodeId={selectedId}
                open={terminalOpen}
                onClose={() => setTerminalOpen(false)}
            />
        </PageContainer>
    );
}
