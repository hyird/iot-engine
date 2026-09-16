import { BugOutlined, FileTextOutlined } from '@ant-design/icons';
import { Alert, Button, Empty, Modal, Space, Tag, Tooltip, Typography } from 'antd';
import dayjs from 'dayjs';
import { useVirtualizer } from '@tanstack/react-virtual';
import { useEffect, useMemo, useRef, useState } from 'react';
import type { DebugAcquisition, DebugPacket } from '@/types/packet_debug';
import {
    type AcquisitionSummary,
    type PacketBranch,
    buildPacketTree,
    describeProtocolPacket,
    flattenAcquisitionPackets,
    summarizeAcquisitions,
} from '@/utils/packet_debug';

interface Props {
    scope: 'device' | 'link';
    protocol: string;
    title: string;
    buttonClassName?: string;
    enabled: boolean;
    inherited?: boolean;
    open: boolean;
    pending: boolean;
    loading: boolean;
    error?: Error | null;
    acquisitions?: DebugAcquisition[];
    onToggle: () => void;
    onOpen: () => void;
    onClose: () => void;
}
export function PacketDebugPanel(props: Props) {
    const active = props.enabled || props.inherited;
    const toggleTitle = props.enabled ? '关闭调试' : '开启调试';
    return (
        <>
            <Space size={2}>
                <Tooltip title={props.inherited ? `${toggleTitle}（链路调试中）` : toggleTitle}>
                    <Button
                        size="small"
                        type="text"
                        className={props.buttonClassName}
                        aria-label={toggleTitle}
                        aria-pressed={props.enabled}
                        icon={
                            <BugOutlined
                                style={active ? { color: 'var(--ant-color-primary)' } : undefined}
                            />
                        }
                        loading={props.pending}
                        onClick={props.onToggle}
                    />
                </Tooltip>
                {active && (
                    <Tooltip title="查看调试报文">
                        <Button
                            size="small"
                            type="text"
                            className={props.buttonClassName}
                            aria-label="查看调试报文"
                            icon={<FileTextOutlined />}
                            onClick={props.onOpen}
                        />
                    </Tooltip>
                )}
            </Space>
            <Modal
                title={props.title}
                width="min(1440px, 96vw)"
                centered
                modalRender={(modal) => (
                    <div
                        style={{
                            resize: 'both',
                            overflow: 'hidden',
                            width: '100%',
                            height: '92dvh',
                            minWidth: 'min(640px, 96vw)',
                            maxWidth: '96vw',
                            minHeight: 360,
                            maxHeight: '96dvh',
                        }}
                    >
                        {modal}
                    </div>
                )}
                destroyOnHidden
                styles={{
                    container: {
                        height: '100%',
                        display: 'flex',
                        flexDirection: 'column',
                        overflow: 'hidden',
                    },
                    body: { flex: 1, minHeight: 0, overflow: 'hidden' },
                    header: { flexShrink: 0 },
                    footer: { flexShrink: 0 },
                }}
                open={props.open}
                onCancel={props.onClose}
                footer={
                    <Space>
                        <Button loading={props.pending} onClick={props.onToggle}>
                            {props.enabled ? '关闭调试' : '开启调试'}
                        </Button>
                        <Button onClick={props.onClose}>关闭窗口</Button>
                    </Space>
                }
            >
                <div className="flex h-full min-h-0 flex-col gap-3">
                    <Alert
                        type={props.enabled || props.inherited ? 'info' : 'warning'}
                        showIcon
                        title={
                            props.enabled
                                ? '调试开关已开启，需手动关闭'
                                : props.inherited
                                  ? '设备独立调试已关闭，链路调试仍在运行'
                                  : '调试已关闭'
                        }
                    />
                    {props.error && (
                        <Alert
                            type="error"
                            title="报文读取失败"
                            description={props.error.message}
                        />
                    )}
                    {props.open && (
                        <PacketTerminal key={`${props.scope}:${props.title}`} {...props} />
                    )}
                </div>
            </Modal>
        </>
    );
}

function PacketTerminal(props: Props) {
    const host = useRef<HTMLDivElement>(null);
    const [following, setFollowing] = useState(true);
    const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(() => new Set());
    const [expandByDefault, setExpandByDefault] = useState(true);
    const rounds = useMemo(
        () => summarizeAcquisitions(props.acquisitions ?? []).reverse(),
        [props.acquisitions]
    );
    const packets = useMemo(() => flattenAcquisitionPackets(rounds).reverse(), [rounds]);
    const rows = props.scope === 'device' ? rounds : packets;
    const virtualizer = useVirtualizer({
        count: rows.length,
        getScrollElement: () => host.current,
        getItemKey: (index) => rows[index].id,
        estimateSize: () => (props.scope === 'device' ? 400 : 110),
        overscan: 5,
    });
    const totalSize = virtualizer.getTotalSize();
    useEffect(() => {
        if (!following || !rows.length) return;
        const frame = requestAnimationFrame(() => {
            if (host.current) host.current.scrollTop = totalSize;
        });
        return () => cancelAnimationFrame(frame);
    }, [following, totalSize, rows]);
    const toggleRound = (id: string) => {
        setFollowing(false);
        setCollapsed((previous) => {
            const next = new Set(previous);
            if (next.has(id)) next.delete(id);
            else next.add(id);
            return next;
        });
    };
    return (
        <>
            <div className="flex flex-wrap items-center justify-between gap-2">
                <Space>
                    <Tag>{props.protocol || '未知协议'}</Tag>
                    <span>
                        {props.loading
                            ? '正在读取…'
                            : props.scope === 'device'
                              ? `${rounds.length} 轮采集`
                              : `${packets.length} 条收发报文`}
                    </span>
                </Space>
                <Space>
                    {props.scope === 'device' && (
                        <>
                            <Button
                                size="small"
                                onClick={() => {
                                    setFollowing(false);
                                    setExpandByDefault(true);
                                    setCollapsed(new Set());
                                }}
                            >
                                展开全部
                            </Button>
                            <Button
                                size="small"
                                onClick={() => {
                                    setFollowing(false);
                                    setExpandByDefault(false);
                                    setCollapsed(new Set());
                                }}
                            >
                                收起全部
                            </Button>
                        </>
                    )}
                    <Button size="small" onClick={() => setFollowing(!following)}>
                        {following ? '暂停跟随' : '回到底部并跟随'}
                    </Button>
                </Space>
            </div>
            <div
                ref={host}
                role="log"
                aria-live="off"
                // biome-ignore lint/a11y/noNoninteractiveTabindex: 日志滚动区需要键盘聚焦，以便用方向键和 PageUp 浏览。
                tabIndex={0}
                aria-label={props.scope === 'device' ? '设备采集日志' : '链路收发日志'}
                className="min-h-0 flex-1 overflow-auto rounded border border-solid border-gray-200 bg-slate-50 p-3 font-mono text-xs dark:border-gray-700 dark:bg-slate-950"
                onWheel={(event) => {
                    if (event.deltaY < 0) setFollowing(false);
                }}
                onTouchStart={() => setFollowing(false)}
                onPointerDown={() => setFollowing(false)}
                onKeyDown={(event) => {
                    if (['ArrowUp', 'PageUp', 'Home'].includes(event.key)) setFollowing(false);
                }}
            >
                {!rows.length && !props.loading && <Empty description="暂无调试报文" />}
                <div style={{ height: totalSize, position: 'relative', width: '100%' }}>
                    {virtualizer.getVirtualItems().map((item) => (
                        <div
                            key={item.key}
                            data-index={item.index}
                            ref={virtualizer.measureElement}
                            style={{
                                position: 'absolute',
                                top: 0,
                                left: 0,
                                width: '100%',
                                transform: `translateY(${item.start}px)`,
                            }}
                        >
                            {props.scope === 'device' ? (
                                <AcquisitionLog
                                    acquisition={rounds[item.index]}
                                    protocol={props.protocol}
                                    expanded={
                                        expandByDefault !== collapsed.has(rounds[item.index].id)
                                    }
                                    onToggle={() => toggleRound(rounds[item.index].id)}
                                />
                            ) : (
                                <PacketLine packet={packets[item.index]} link />
                            )}
                        </div>
                    ))}
                </div>
            </div>
        </>
    );
}

function AcquisitionLog({
    acquisition,
    protocol,
    expanded,
    onToggle,
}: {
    acquisition: AcquisitionSummary;
    protocol: string;
    expanded: boolean;
    onToggle: () => void;
}) {
    const branches = useMemo(() => buildPacketTree(acquisition.packets), [acquisition.packets]);
    const states = {
        running: '采集中',
        success: '成功',
        partial: '部分失败',
        failed: '失败',
        unreported: '未上报轮次',
    };
    const storage: Record<string, string> = {
        stored: '已入库',
        pending: '待入库',
        failed: '入库失败',
        skipped: '未保存',
    };
    return (
        <section className="mb-3 border-b border-solid border-gray-200 pb-3 dark:border-gray-700">
            <button
                type="button"
                aria-expanded={expanded}
                onClick={onToggle}
                className="flex w-full cursor-pointer flex-wrap items-center gap-2 border-0 bg-transparent p-1 text-left text-inherit"
            >
                <span>{expanded ? '▼' : '▶'}</span>
                <time className="whitespace-nowrap tabular-nums">
                    {formatTime(acquisition.startedAt)}
                </time>
                <strong>{protocol} · 采集轮次</strong>
                <Tag
                    color={
                        acquisition.state === 'success'
                            ? 'success'
                            : acquisition.state === 'failed'
                              ? 'error'
                              : 'processing'
                    }
                >
                    {states[acquisition.state]}
                </Tag>
                <span>
                    发送 {acquisition.sent} / 接收 {acquisition.received}
                </span>
                <span>{Math.max(0, acquisition.updatedAt - acquisition.startedAt)} ms</span>
                {acquisition.storage_status && (
                    <span>{storage[acquisition.storage_status] ?? acquisition.storage_status}</span>
                )}
                <Tooltip title={acquisition.id}>
                    <span className="text-gray-500">#{acquisition.id.slice(0, 8)}</span>
                </Tooltip>
            </button>
            {expanded && (
                <div className="ml-2 border-l border-solid border-gray-300 pl-3 dark:border-gray-600">
                    {branches.map((branch) => (
                        <PacketTree key={branch.packet.id} branch={branch} protocol={protocol} />
                    ))}
                    {!branches.length && <div className="py-2 text-gray-500">本轮暂无收发报文</div>}
                    <div className="py-2">
                        <strong>└ 本轮解析结果</strong>
                        <div className="mt-2 pl-4">
                            {acquisition.parsed_json ? (
                                <ParsedHistory packet={acquisition} />
                            ) : (
                                <span className="text-gray-500">暂无解析结果</span>
                            )}
                        </div>
                    </div>
                </div>
            )}
        </section>
    );
}

function PacketTree({ branch, protocol }: { branch: PacketBranch; protocol: string }) {
    return (
        <div>
            <div className="pt-2 font-semibold">
                ├ {describeProtocolPacket(protocol, branch.packet)}
            </div>
            <div className="pl-3">
                <PacketLine packet={branch.packet} />
            </div>
            {branch.children.length > 0 && (
                <div className="ml-3 border-l border-solid border-gray-300 pl-3 dark:border-gray-600">
                    {branch.children.map((child) => (
                        <PacketTree key={child.packet.id} branch={child} protocol={protocol} />
                    ))}
                </div>
            )}
        </div>
    );
}

function formatTime(time: string | number) {
    return dayjs(Number(time)).format('YYYY-MM-DD HH:mm:ss.SSS');
}

function PacketLine({
    packet,
    protocol,
    link = false,
}: {
    packet: DebugPacket;
    protocol?: string;
    link?: boolean;
}) {
    const sending = packet.direction === 'TX' || packet.direction === 'TX_ATTEMPT';
    return (
        <article className="min-w-0 py-2">
            <div className="flex flex-wrap items-center gap-2">
                <time className="whitespace-nowrap tabular-nums">{formatTime(packet.time_ms)}</time>
                <span
                    className={sending ? 'font-bold text-blue-600' : 'font-bold text-emerald-600'}
                >
                    {sending ? '↑ 发送' : '↓ 接收'}
                </span>
                {link && (
                    <span className="break-all text-gray-500">
                        {packet.address || '对端未知'} · {packet.device_id || '未识别设备'}
                    </span>
                )}
                <PacketStatuses packet={packet} transportOnly={link} />
            </div>
            {!link && protocol && (
                <div className="my-1 break-words">
                    {describeProtocolPacket(protocol ?? '', packet)}
                </div>
            )}
            <Typography.Paragraph
                copyable={{ text: packet.payload_hex }}
                className="mb-0 break-all font-mono text-xs"
            >
                {packet.payload_hex || '（空报文）'}
            </Typography.Paragraph>
            {packet.reason && <div className="break-words text-red-500">{packet.reason}</div>}
        </article>
    );
}

function PacketStatuses({
    packet,
    transportOnly = false,
}: {
    packet: DebugPacket;
    transportOnly?: boolean;
}) {
    const groups: [string | undefined, Record<string, string>][] = [
        [
            packet.transport_status,
            { sending: '发送中', sent: '已发送', received: '已接收', failed: '发送失败' },
        ],
        [packet.response_status, { waiting: '等待应答', success: '应答成功', failed: '应答失败' }],
        [packet.parse_status, { pending: '待解析', success: '解析成功', failed: '解析失败' }],
    ];
    return (
        <Tooltip title={packet.reason || undefined}>
            <div className="flex flex-wrap items-center gap-1">
                {(transportOnly ? groups.slice(0, 1) : groups.slice(0, 3)).map(
                    ([status, labels]) =>
                        status && labels[status] ? (
                            <Tag
                                key={labels[status]}
                                color={
                                    status === 'failed'
                                        ? 'error'
                                        : status === 'success'
                                          ? 'success'
                                          : 'default'
                                }
                            >
                                {labels[status]}
                            </Tag>
                        ) : null
                )}
            </div>
        </Tooltip>
    );
}

function ParsedHistory({ packet }: { packet: { parsed_json?: string; history_id?: string } }) {
    if (!packet.parsed_json) return <Typography.Text type="secondary">—</Typography.Text>;
    let values: Record<string, unknown>;
    try {
        const parsed = JSON.parse(packet.parsed_json);
        values = parsed?.values ?? parsed;
        if (!values || typeof values !== 'object' || Array.isArray(values))
            throw new Error('invalid parsed values');
    } catch {
        return <Typography.Text type="secondary">解析数据格式无效</Typography.Text>;
    }
    return (
        <div className="flex flex-wrap gap-x-6 gap-y-1">
            {packet.history_id && (
                <Tooltip title={packet.history_id}>
                    <Typography.Text type="secondary">
                        历史记录 · {packet.history_id.slice(0, 8)}
                    </Typography.Text>
                </Tooltip>
            )}
            {Object.entries(values).map(([key, point]) => {
                const item =
                    typeof point === 'object' && point !== null
                        ? (point as Record<string, unknown>)
                        : { value: point };
                const value =
                    item.value === null || item.value === undefined
                        ? '—'
                        : typeof item.value === 'object'
                          ? JSON.stringify(item.value)
                          : String(item.value);
                return (
                    <div key={key} className="break-all text-xs">
                        <span className="text-gray-500">{String(item.name ?? key)}：</span>
                        {value} {String(item.unit ?? '')}
                    </div>
                );
            })}
        </div>
    );
}
