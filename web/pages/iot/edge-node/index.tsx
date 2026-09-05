import {
    ApartmentOutlined,
    CheckOutlined,
    CodeOutlined,
    DeleteOutlined,
    DownloadOutlined,
    EditOutlined,
    EyeOutlined,
    GlobalOutlined,
    PlusOutlined,
    ReloadOutlined,
    SyncOutlined,
    UploadOutlined,
} from '@ant-design/icons';
import { create, fromBinary, toBinary } from '@bufbuild/protobuf';
import { FitAddon } from '@xterm/addon-fit';
import { WebglAddon } from '@xterm/addon-webgl';
import { Terminal } from '@xterm/xterm';
import {
    Alert,
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
    Progress,
    Result,
    Select,
    Skeleton,
    Space,
    Switch,
    Table,
    Tabs,
    Tag,
    Tooltip,
    TreeSelect,
    Upload,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import '@xterm/xterm/css/xterm.css';
import { useEffect, useMemo, useRef, useState } from 'react';
import DeviceCard, { type DeviceCardItem } from '@/components/DeviceCard';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import {
    WebTerminalDataSchema,
    WebTerminalFrameSchema,
    WebTerminalResizeSchema,
} from '@/generated/edge/terminal-pb';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { formatDateTime } from '@/utils/dateTime';
import { validateForm } from '@/utils/validation';
import { getEdgeDetail, getTerminalTicket } from './edge-node.client';
import EdgeNodeGroupPanel from './EdgeNodeGroupPanel';
import EdgeVpnPanel from './EdgeVpnPanel';
import { normalizeReportedNetwork, physicalNetworkInterfaces } from './edge-node.network';
import {
    firmwareUpgradeSchema,
    networkInterfaceSchema,
    networkSchema,
    nodeNameSchema,
} from './edge-node.schema';
import {
    useDeviceConfigSyncMutation,
    useEdgeDeleteMutation,
    useEdgeDetail,
    useEdgeGroupTree,
    useEdgeList,
    useEdgeLogs,
    useEnrollmentMutation,
    useFirmwareUpgradeMutation,
    useLogLevelMutation,
    useNetworkMutation,
    useNodeGroupMutation,
    useNodeNameMutation,
} from './edge-node.service';
import type { Edge } from './edge-node.types';
import {
    useWindowsVpnConfigCreate,
    useWindowsVpnConfigDelete,
    useWindowsVpnConfigDownload,
    useWindowsVpnConfigs,
} from './edge-node.vpn.service';
import type { EdgeVpn } from './edge-node.vpn.types';

type NetworkDraftItem = Edge.NetworkConfig & {
    sourceName?: string;
    original: boolean;
    dirty: boolean;
    up?: boolean;
};

const EDGE_CARD_GRID_CLASS = 'grid grid-cols-1 gap-3 xl:grid-cols-2 2xl:grid-cols-4';
const EDGE_DETAIL_DRAWER_Z_INDEX = 1000;
const EDGE_ACTION_MODAL_Z_INDEX = EDGE_DETAIL_DRAWER_Z_INDEX + 100;

function downloadClientConfig(result: EdgeVpn.ClientConfig) {
    const safeName = result.name.replace(/[^A-Za-z0-9._-]+/g, '-').replace(/^-+|-+$/g, '');
    const fileName = `${safeName || 'wireguard'}.conf`;
    const url = URL.createObjectURL(
        new Blob([result.config], { type: 'text/plain;charset=utf-8' })
    );
    const link = document.createElement('a');
    link.href = url;
    link.download = fileName;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
}

function groupSelectOptions(
    groups: Edge.GroupTreeItem[]
): { value: string; title: string; disabled: boolean; children?: ReturnType<typeof groupSelectOptions> }[] {
    return groups.map((group) => ({
        value: group.id,
        title: group.name,
        disabled: group.status === 'disabled',
        children: group.children?.length ? groupSelectOptions(group.children) : undefined,
    }));
}

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
        { key: 'group', label: '分组', children: node.groupName || '未分组' },
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
                ? `${mobile.signal.percent}%${
                      mobile.signal.rssiDbm !== -1 ? ` · ${mobile.signal.rssiDbm} dBm` : ''
                  }`
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
                                  `${percent ?? 0}% · ${formatBytes(
                                      firmware.downloadedBytes
                                  )} / ${formatBytes(firmware.totalBytes)}`
                              }
                          />
                      ),
                      span: 2,
                  } satisfies DeviceCardItem,
              ]
            : []),
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
    const [connectionEnded, setConnectionEnded] = useState(false);
    const [connectionAttempt, setConnectionAttempt] = useState(0);
    const socketRef = useRef<WebSocket | null>(null);
    const terminalHostRef = useRef<HTMLDivElement | null>(null);

    useEffect(() => {
        // A manual retry creates a new socket and terminal session.
        void connectionAttempt;
        const host = terminalHostRef.current;
        if (!open || !nodeId || !host) return;

        setConnectionEnded(false);
        let disposed = false;
        let fitFrame: number | undefined;
        let resizeTimer: number | undefined;
        let inputTimer: number | undefined;
        let outputFrame: number | undefined;
        let lastSentSize = '';
        const encoder = new TextEncoder();
        const pendingInput: Uint8Array[] = [];
        let pendingInputBytes = 0;
        let terminalReady = false;
        const pendingOutput: Uint8Array[] = [];
        const outputLimitNotice = encoder.encode('\r\n[终端输出过快，已省略较早内容]\r\n');
        let pendingOutputBytes = 0;
        let outputWriting = false;
        let outputNoticeQueued = false;
        let terminalCloseReason = '';
        const terminal = new Terminal({
            cursorBlink: true,
            fontFamily: "'Cascadia Mono', 'SFMono-Regular', Consolas, 'Liberation Mono', monospace",
            fontSize: 14,
            lineHeight: 1.2,
            scrollback: 2_000,
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
            const webglAddon = new WebglAddon();
            webglAddon.onContextLoss(() => webglAddon.dispose());
            terminal.loadAddon(webglAddon);
        } catch {
            // Canvas renderer remains available when WebGL is unsupported.
        }

        const fitTerminal = () => {
            fitFrame = undefined;
            if (host.clientWidth === 0 || host.clientHeight === 0) return;
            fitAddon.fit();
            const socket = socketRef.current;
            const sizeKey = `${terminal.cols}:${terminal.rows}`;
            if (
                terminalReady &&
                socket?.readyState === WebSocket.OPEN &&
                sizeKey !== lastSentSize
            ) {
                const resize = create(WebTerminalResizeSchema, {
                    columns: terminal.cols,
                    rows: terminal.rows,
                });
                socket.send(
                    toBinary(
                        WebTerminalFrameSchema,
                        create(WebTerminalFrameSchema, {
                            payload: { case: 'resize', value: resize },
                        })
                    )
                );
                lastSentSize = sizeKey;
            }
        };
        const scheduleFit = () => {
            if (fitFrame !== undefined) return;
            fitFrame = window.requestAnimationFrame(fitTerminal);
        };
        const flushInput = () => {
            inputTimer = undefined;
            const socket = socketRef.current;
            if (pendingInput.length === 0) return;
            if (!terminalReady || socket?.readyState !== WebSocket.OPEN) {
                // The terminal is focused before the device has confirmed PTY creation.
                // Hold keystrokes until TerminalOpened reaches the browser as Ready.
                if (
                    socket === null ||
                    socket.readyState === WebSocket.CONNECTING ||
                    (socket.readyState === WebSocket.OPEN && !terminalReady)
                ) {
                    if (inputTimer === undefined) inputTimer = window.setTimeout(flushInput, 50);
                    return;
                }
                pendingInput.length = 0;
                pendingInputBytes = 0;
                return;
            }
            const size = pendingInput.reduce((total, chunk) => total + chunk.byteLength, 0);
            const payload = new Uint8Array(size);
            let offset = 0;
            for (const chunk of pendingInput.splice(0)) {
                payload.set(chunk, offset);
                offset += chunk.byteLength;
            }
            pendingInputBytes = 0;
            const data = create(WebTerminalDataSchema, { data: payload });
            socket.send(
                toBinary(
                    WebTerminalFrameSchema,
                    create(WebTerminalFrameSchema, {
                        payload: { case: 'data', value: data },
                    })
                )
            );
        };
        const takeOutput = (limit: number) => {
            if (pendingOutput.length === 0) return undefined;
            const parts: Uint8Array[] = [];
            let size = 0;
            while (pendingOutput.length > 0 && size < limit) {
                const chunk = pendingOutput[0];
                const remaining = limit - size;
                if (chunk.byteLength <= remaining) {
                    parts.push(chunk);
                    pendingOutput.shift();
                    pendingOutputBytes -= chunk.byteLength;
                    size += chunk.byteLength;
                } else {
                    parts.push(chunk.slice(0, remaining));
                    pendingOutput[0] = chunk.slice(remaining);
                    pendingOutputBytes -= remaining;
                    size += remaining;
                }
            }
            outputNoticeQueued = pendingOutput.some((chunk) => chunk === outputLimitNotice);
            if (parts.length === 1) return parts[0];
            const payload = new Uint8Array(size);
            let offset = 0;
            for (const chunk of parts) {
                payload.set(chunk, offset);
                offset += chunk.byteLength;
            }
            return payload;
        };
        const scheduleOutput = () => {
            if (disposed || outputWriting || outputFrame !== undefined) return;
            outputFrame = window.requestAnimationFrame(flushOutput);
        };
        const trimOutputBacklog = () => {
            const maxBacklogBytes = 1024 * 1024;
            while (pendingOutputBytes > maxBacklogBytes && pendingOutput.length > 0) {
                const chunk = pendingOutput.shift();
                pendingOutputBytes -= chunk?.byteLength ?? 0;
            }
            if (!outputNoticeQueued) {
                pendingOutput.unshift(outputLimitNotice);
                pendingOutputBytes += outputLimitNotice.byteLength;
                outputNoticeQueued = true;
            }
        };
        function flushOutput() {
            outputFrame = undefined;
            if (disposed || outputWriting || pendingOutput.length === 0) return;
            const payload = takeOutput(32 * 1024);
            if (!payload || payload.byteLength === 0) return;
            outputWriting = true;
            terminal.write(payload, () => {
                outputWriting = false;
                scheduleOutput();
            });
        }
        const appendOutput = (payload: Uint8Array) => {
            if (disposed || payload.byteLength === 0) return;
            pendingOutput.push(payload);
            pendingOutputBytes += payload.byteLength;
            if (pendingOutputBytes > 1024 * 1024) trimOutputBacklog();
            scheduleOutput();
        };
        const resizeObserver = new ResizeObserver(() => {
            if (resizeTimer !== undefined) window.clearTimeout(resizeTimer);
            resizeTimer = window.setTimeout(() => {
                resizeTimer = undefined;
                scheduleFit();
            }, 100);
        });
        const restoreTerminalFocus = () => {
            if (disposed || document.visibilityState !== 'visible') return;
            scheduleFit();
            terminal.focus();
        };
        resizeObserver.observe(host);
        window.addEventListener('focus', restoreTerminalFocus);
        document.addEventListener('visibilitychange', restoreTerminalFocus);
        scheduleFit();
        const input = terminal.onData((data) => {
            const chunk = encoder.encode(data);
            pendingInputBytes += chunk.byteLength;
            // A held buffer must stay bounded: drop the oldest keystrokes rather than
            // grow without limit if the handshake never completes.
            while (pendingInputBytes > 256 * 1024 && pendingInput.length > 0) {
                pendingInputBytes -= pendingInput.shift()?.byteLength ?? 0;
            }
            pendingInput.push(chunk);
            if (inputTimer === undefined) inputTimer = window.setTimeout(flushInput, 8);
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
                    if (disposed) return;
                    setState('已连接，正在启动终端…');
                    terminal.focus();
                };
                socket.onmessage = (event) => {
                    if (disposed) return;
                    if (!(event.data instanceof ArrayBuffer)) {
                        setState('终端协议错误');
                        socket.close(1003, 'terminal frames must use protobuf');
                        return;
                    }
                    try {
                        const frame = fromBinary(
                            WebTerminalFrameSchema,
                            new Uint8Array(event.data)
                        );
                        if (frame.payload.case === 'ready') {
                            terminalReady = true;
                            setState('已连接');
                            scheduleFit();
                            terminal.focus();
                            flushInput();
                        } else if (frame.payload.case === 'data') {
                            appendOutput(frame.payload.value.data);
                        } else if (frame.payload.case === 'close') {
                            terminalCloseReason = frame.payload.value.reason || '终端已关闭';
                            setState(terminalCloseReason);
                            socket.close(1000, 'terminal closed');
                        } else {
                            setState('终端协议错误');
                            socket.close(1002, 'invalid terminal protobuf');
                        }
                    } catch {
                        setState('终端协议错误');
                        socket.close(1002, 'invalid terminal protobuf');
                    }
                };
                socket.onerror = () => {
                    if (disposed) return;
                    terminalCloseReason ||= '终端连接失败';
                    setState(terminalCloseReason);
                };
                socket.onclose = (event) => {
                    if (disposed) return;
                    terminalReady = false;
                    terminal.options.disableStdin = true;
                    pendingInput.length = 0;
                    pendingInputBytes = 0;
                    if (inputTimer !== undefined) window.clearTimeout(inputTimer);
                    inputTimer = undefined;
                    setConnectionEnded(true);
                    setState(terminalCloseReason || event.reason || '终端已关闭');
                };
            })
            .catch(() => {
                if (disposed) return;
                terminal.options.disableStdin = true;
                pendingInput.length = 0;
                pendingInputBytes = 0;
                if (inputTimer !== undefined) window.clearTimeout(inputTimer);
                inputTimer = undefined;
                setConnectionEnded(true);
                setState('无法建立终端连接');
            });
        return () => {
            disposed = true;
            if (fitFrame !== undefined) window.cancelAnimationFrame(fitFrame);
            if (resizeTimer !== undefined) window.clearTimeout(resizeTimer);
            if (inputTimer !== undefined) window.clearTimeout(inputTimer);
            if (outputFrame !== undefined) window.cancelAnimationFrame(outputFrame);
            resizeObserver.disconnect();
            window.removeEventListener('focus', restoreTerminalFocus);
            document.removeEventListener('visibilitychange', restoreTerminalFocus);
            input.dispose();
            socketRef.current?.close();
            socketRef.current = null;
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
                        <Button size="small" onClick={() => setConnectionAttempt((value) => value + 1)}>
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

export default function EdgeNodePage() {
    const { has } = usePermissions();
    const canQuery = has('iot:edge:query');
    const canEdit = has('iot:edge:edit');
    const canConfig = has('iot:edge:config');
    const canFirmware = has('iot:edge:firmware');
    const canTerminal = has('iot:edge:terminal');
    const canQueryVpnConfigs = has('iot:vpn:query') && has('iot:edge:query');
    const canCreateVpnConfig = has('iot:vpn:enroll') && has('iot:edge:query');
    const canDeleteVpnConfig = has('iot:vpn:revoke');
    const canManageVpnConfigs = canQueryVpnConfigs || canCreateVpnConfig;
    const [pagination, setPagination] = useState({ page: 1, pageSize: 20 });
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
    const [logLevel, setLogLevel] = useState<Edge.LogLevel>();
    const [nodeLogLevel, setNodeLogLevel] = useState<Edge.LogLevel>('info');
    const [networkOpen, setNetworkOpen] = useState(false);
    const [firmwareOpen, setFirmwareOpen] = useState(false);
    const [firmwareUploadProgress, setFirmwareUploadProgress] =
        useState<Edge.FirmwareUploadProgress>();
    const [terminalOpen, setTerminalOpen] = useState(false);
    const [clientConfigOpen, setClientConfigOpen] = useState(false);
    const [networkForm] = Form.useForm<Edge.NetworkConfig>();
    const [firmwareForm] = Form.useForm<Edge.FirmwareUpgradeDto>();
    const [nameForm] = Form.useForm<Edge.NameDto>();
    const [groupForm] = Form.useForm<Edge.GroupDto>();
    const [clientConfigForm] = Form.useForm<EdgeVpn.ClientConfigCreateDto>();
    const networkMode = Form.useWatch('mode', networkForm);
    const networkBridge = Form.useWatch('bridge', networkForm);
    const firmwareFile = Form.useWatch('file', firmwareForm);
    const { message, modal } = App.useApp();
    const { run: search } = useDebounceFn((value: string) => {
        setKeyword(value);
        setPagination((current) => ({ ...current, page: 1 }));
    }, 300);
    const query = {
        ...pagination,
        keyword: keyword || undefined,
        status,
        groupId: selectedGroupId ?? undefined,
    };
    const { data, isLoading } = useEdgeList(query, canQuery);
    const nodes: Edge.Node[] = data?.list ?? [];
    const { data: edgeGroups = [] } = useEdgeGroupTree();
    const edgeGroupOptions = useMemo(() => groupSelectOptions(edgeGroups), [edgeGroups]);
    const { data: detail, isLoading: detailLoading } = useEdgeDetail(selectedId);
    const {
        data: eventLogs,
        isFetching: eventLogsLoading,
        refetch: refreshEventLogs,
    } = useEdgeLogs(
        selectedId,
        { limit: 48, level: logLevel },
        Boolean(
            selectedId && detailTab === 'events' && detail?.status.online && detail.capability.logs
        )
    );
    const {
        data: systemLogs,
        isFetching: systemLogsLoading,
        refetch: refreshSystemLogs,
    } = useEdgeLogs(
        selectedId,
        { limit: 48, source: 'system' },
        Boolean(
            selectedId && detailTab === 'system' && detail?.status.online && detail.capability.logs
        )
    );
    const enrollment = useEnrollmentMutation();
    const edgeDelete = useEdgeDeleteMutation();
    const nodeName = useNodeNameMutation();
    const nodeGroup = useNodeGroupMutation();
    const network = useNetworkMutation();
    const deviceConfigSync = useDeviceConfigSyncMutation();
    const firmwareUpgrade = useFirmwareUpgradeMutation();
    const logLevelControl = useLogLevelMutation();
    const windowsVpnConfig = useWindowsVpnConfigCreate();
    const windowsVpnConfigDelete = useWindowsVpnConfigDelete();
    const windowsVpnConfigDownload = useWindowsVpnConfigDownload();
    const {
        data: windowsVpnConfigs = [],
        isLoading: windowsVpnConfigsLoading,
        isFetching: windowsVpnConfigsFetching,
        refetch: refreshWindowsVpnConfigs,
    } = useWindowsVpnConfigs(clientConfigOpen && canQueryVpnConfigs);

    useEffect(() => {
        if (!selectedId) return;
        const exists = data?.list.some((node) => node.id === selectedId) ?? true;
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

    const showClientConfig = () => {
        windowsVpnConfig.reset();
        clientConfigForm.resetFields();
        setClientConfigOpen(true);
    };

    const submitClientConfig = (values: EdgeVpn.ClientConfigCreateDto) => {
        windowsVpnConfig.mutate(values, {
            onSuccess: (result) => {
                downloadClientConfig(result);
                setClientConfigOpen(false);
            },
        });
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
                            `${percent ?? 0}% · ${formatBytes(item.downloadedBytes)} / ${formatBytes(
                                item.totalBytes
                            )}`
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

    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-center justify-between gap-3">
                    <div>
                        <h2 className="m-0 text-lg font-semibold text-slate-900">边缘节点</h2>
                        <p className="m-0 mt-1 text-xs text-slate-500">
                            节点平台入口由 LuCI/UCI 管理，可同时连接多个 HTTP(S) 平台
                        </p>
                    </div>
                    <Space wrap>
                        <EdgeNodeGroupPanel
                            selectedGroupId={selectedGroupId}
                            canManageGroup={canEdit}
                            onSelect={(groupId) => {
                                setSelectedGroupId(groupId);
                                setPagination((current) => ({ ...current, page: 1 }));
                            }}
                        />
                        {canManageVpnConfigs && (
                            <Button icon={<DownloadOutlined />} onClick={showClientConfig}>
                                VPN 配置
                            </Button>
                        )}
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
                        {nodes.map((node) => {
                            const status = node.status;
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
                                                <span className="min-w-0 flex-1 whitespace-normal break-words pr-1 text-left leading-5">
                                                    {node.name || node.hostname || '未命名节点'}
                                                    <span className="ml-2 whitespace-nowrap text-xs font-normal text-slate-400">
                                                        IMEI：{node.imei}
                                                    </span>
                                                </span>
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
                                                    <Tag
                                                        color="purple"
                                                        className="!mr-0 !rounded-md"
                                                    >
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
                                    children: <EdgeVpnPanel node={detail} />,
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
                                                                `${percent ?? 0}% · ${formatBytes(
                                                                    detail.firmware.downloadedBytes
                                                                )} / ${formatBytes(
                                                                    detail.firmware.totalBytes
                                                                )}`
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
                title={`${networkOpen ? (editingNetwork ? '编辑网络接口' : '添加网络接口') : '网络接口'}${
                    networkNode ? ` · ${networkNode.name || networkNode.imei}` : ''
                }`}
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

            <Modal
                open={clientConfigOpen}
                zIndex={EDGE_ACTION_MODAL_Z_INDEX}
                title="VPN 配置管理"
                width={860}
                footer={null}
                onCancel={() => {
                    if (!windowsVpnConfig.isPending) setClientConfigOpen(false);
                }}
                closable={!windowsVpnConfig.isPending}
                keyboard={!windowsVpnConfig.isPending}
                maskClosable={!windowsVpnConfig.isPending}
                destroyOnHidden
            >
                <Alert
                    type="info"
                    showIcon
                    className="mb-4"
                    message="每台 Windows 设备必须单独生成一份配置"
                    description="配置可重复下载；每次下载都会写入当前账户可访问的全部边缘节点虚拟网段。"
                />
                {canCreateVpnConfig && (
                    <Form
                        form={clientConfigForm}
                        layout="inline"
                        className="mb-4"
                        onFinish={submitClientConfig}
                    >
                        <Form.Item
                            label="客户端设备"
                            name="name"
                            className="min-w-[260px] flex-1"
                            rules={[
                                { required: true, message: '请输入客户端设备名称' },
                                { max: 100, message: '客户端名称不能超过 100 个字符' },
                            ]}
                        >
                            <Input placeholder="例如：张三办公电脑" maxLength={100} />
                        </Form.Item>
                        <Form.Item>
                            <Button
                                type="primary"
                                htmlType="submit"
                                icon={<DownloadOutlined />}
                                loading={windowsVpnConfig.isPending}
                            >
                                新增并下载
                            </Button>
                        </Form.Item>
                    </Form>
                )}
                <Flex justify="space-between" align="center" className="mb-2">
                    <span className="text-sm font-medium text-slate-800">客户端配置</span>
                    {canQueryVpnConfigs && (
                        <Button
                            type="text"
                            size="small"
                            icon={<ReloadOutlined />}
                            loading={windowsVpnConfigsFetching}
                            onClick={() => void refreshWindowsVpnConfigs()}
                        >
                            刷新
                        </Button>
                    )}
                </Flex>
                {canQueryVpnConfigs ? (
                    <Table<EdgeVpn.ClientConfigSummary>
                        rowKey="id"
                        size="small"
                        loading={windowsVpnConfigsLoading}
                        pagination={false}
                        dataSource={windowsVpnConfigs}
                        scroll={{ x: 'max-content' }}
                        locale={{ emptyText: '暂无客户端 VPN 配置' }}
                        columns={[
                            { title: '设备名称', dataIndex: 'name', width: 180 },
                            { title: 'VPN 地址', dataIndex: 'assignedIpv4', width: 130 },
                            {
                                title: '可访问虚拟网段',
                                dataIndex: 'allowedRoutes',
                                render: (routes: string[]) =>
                                    routes.length ? routes.join('、') : '-',
                            },
                            {
                                title: '最近连接',
                                dataIndex: 'lastHandshakeAt',
                                width: 170,
                                render: (value) => (value ? formatDateTime(value) : '从未连接'),
                            },
                            {
                                title: '创建时间',
                                dataIndex: 'createdAt',
                                width: 170,
                                render: (value) => formatDateTime(value),
                            },
                            {
                                title: '操作',
                                key: 'actions',
                                width: 140,
                                fixed: 'right',
                                render: (_, item) => (
                                    <Flex gap={4}>
                                        <Button
                                            type="link"
                                            size="small"
                                            loading={
                                                windowsVpnConfigDownload.isPending &&
                                                windowsVpnConfigDownload.variables === item.id
                                            }
                                            onClick={() =>
                                                windowsVpnConfigDownload.mutate(item.id, {
                                                    onSuccess: downloadClientConfig,
                                                })
                                            }
                                        >
                                            下载
                                        </Button>
                                        {canDeleteVpnConfig && (
                                            <Popconfirm
                                                title={`删除 ${item.name} 的 VPN 配置？`}
                                                description="删除后该客户端会立即失去 VPN 访问权限，已下载文件也无法再使用。"
                                                okText="删除"
                                                okButtonProps={{ danger: true }}
                                                onConfirm={() =>
                                                    windowsVpnConfigDelete.mutate(item.id)
                                                }
                                            >
                                                <Button
                                                    type="link"
                                                    danger
                                                    size="small"
                                                    loading={
                                                        windowsVpnConfigDelete.isPending &&
                                                        windowsVpnConfigDelete.variables === item.id
                                                    }
                                                >
                                                    删除
                                                </Button>
                                            </Popconfirm>
                                        )}
                                    </Flex>
                                ),
                            },
                        ]}
                    />
                ) : (
                    <Alert type="warning" showIcon message="您没有 VPN 配置查询权限" />
                )}
            </Modal>

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
