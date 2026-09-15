import { BugOutlined, FileTextOutlined } from '@ant-design/icons';
import {
    Alert,
    Button,
    Empty,
    Modal,
    Segmented,
    Space,
    Table,
    Tag,
    Tooltip,
    Typography,
} from 'antd';
import dayjs from 'dayjs';
import { useEffect, useMemo, useRef, useState } from 'react';
import type { DebugAcquisition, DebugPacket } from '@/types/packet_debug';
import {
    type AcquisitionSummary,
    flattenAcquisitionPackets,
    summarizeAcquisitions,
} from '@/utils/packet_debug';

interface Props {
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
    const tableHost = useRef<HTMLDivElement>(null);
    const [tableHeight, setTableHeight] = useState(400);
    const [modalReady, setModalReady] = useState(false);
    const [view, setView] = useState('acquisitions');
    const acquisitions = useMemo(
        () => summarizeAcquisitions(props.acquisitions ?? []),
        [props.acquisitions]
    );
    const packets = useMemo(() => flattenAcquisitionPackets(acquisitions), [acquisitions]);
    useEffect(() => {
        const host = tableHost.current;
        if (!props.open || !modalReady || !host) return;
        const measure = () => setTableHeight(Math.max(80, host.clientHeight - 90));
        const observer = new ResizeObserver(measure);
        observer.observe(host);
        measure();
        return () => observer.disconnect();
    }, [props.open, modalReady]);
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
                afterOpenChange={setModalReady}
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
                    <Segmented
                        value={view}
                        onChange={setView}
                        options={[
                            { label: '按采集轮次', value: 'acquisitions' },
                            { label: '逐条报文', value: 'packets' },
                        ]}
                    />
                    <div ref={tableHost} className="min-h-0 flex-1">
                        {view === 'acquisitions' ? (
                            <AcquisitionTable
                                acquisitions={acquisitions}
                                loading={props.loading}
                                height={tableHeight}
                            />
                        ) : (
                            <Table<DebugPacket>
                                size="small"
                                rowKey="id"
                                loading={props.loading}
                                dataSource={packets}
                                pagination={{ pageSize: 20, showSizeChanger: false }}
                                scroll={{ x: 1200, y: tableHeight }}
                                locale={{
                                    emptyText: (
                                        <Empty description="暂无调试报文；边缘节点需支持调试并连接平台" />
                                    ),
                                }}
                                columns={[
                                    {
                                        title: '时间',
                                        width: 228,
                                        onCell: () => ({ style: { verticalAlign: 'top' } }),
                                        render: (_, row) => (
                                            <div className="whitespace-nowrap tabular-nums">
                                                {dayjs(Number(row.time_ms)).format(
                                                    'YYYY-MM-DD HH:mm:ss.SSS'
                                                )}
                                            </div>
                                        ),
                                    },
                                    {
                                        title: '方向',
                                        width: 80,
                                        render: (_, row) => (
                                            <Tag>
                                                {row.direction === 'TX_ATTEMPT' ||
                                                row.direction === 'TX'
                                                    ? '发送'
                                                    : '接收'}
                                            </Tag>
                                        ),
                                    },
                                    {
                                        title: '状态',
                                        width: 132,
                                        render: (_, row) => <PacketStatuses packet={row} />,
                                    },
                                    {
                                        title: '原始报文 HEX',
                                        width: 440,
                                        render: (_, row) => (
                                            <>
                                                <div className="text-xs text-gray-500">
                                                    {row.device_id || '未识别设备'} {row.address}
                                                </div>
                                                {row.reply_to_packet_id && (
                                                    <Tooltip title={row.reply_to_packet_id}>
                                                        <Typography.Text type="secondary">
                                                            关联接收报文
                                                        </Typography.Text>
                                                    </Tooltip>
                                                )}
                                                <Typography.Paragraph
                                                    copyable={{ text: row.payload_hex }}
                                                    ellipsis={{
                                                        rows: 3,
                                                        expandable: 'collapsible',
                                                        symbol: (expanded) =>
                                                            expanded ? '收起' : '展开',
                                                    }}
                                                    className="mb-0 break-all font-mono text-xs"
                                                >
                                                    {row.payload_hex}
                                                </Typography.Paragraph>
                                            </>
                                        ),
                                    },
                                    {
                                        title: '对应历史解析数据',
                                        width: 320,
                                        render: (_, row) => <ParsedHistory packet={row} />,
                                    },
                                ]}
                            />
                        )}
                    </div>
                </div>
            </Modal>
        </>
    );
}

function AcquisitionTable({
    acquisitions,
    loading,
    height,
}: {
    acquisitions: AcquisitionSummary[];
    loading: boolean;
    height: number;
}) {
    return (
        <Table<AcquisitionSummary>
            rowKey="id"
            size="small"
            loading={loading}
            dataSource={acquisitions}
            pagination={{ pageSize: 10, showSizeChanger: false }}
            scroll={{ x: 1000, y: height }}
            locale={{ emptyText: <Empty description="暂无采集轮次" /> }}
            expandable={{
                expandedRowRender: (round) => <AcquisitionDetails acquisition={round} />,
            }}
            columns={[
                {
                    title: '开始时间',
                    width: 228,
                    render: (_, round) => (
                        <span className="whitespace-nowrap tabular-nums">
                            {dayjs(round.startedAt).format('YYYY-MM-DD HH:mm:ss.SSS')}
                        </span>
                    ),
                },
                {
                    title: '轮次耗时',
                    width: 112,
                    render: (_, round) => `${Math.max(0, round.updatedAt - round.startedAt)} ms`,
                },
                {
                    title: '收发明细',
                    width: 164,
                    render: (_, round) => (
                        <Space size={4}>
                            <Tag>发 {round.sent}</Tag>
                            <Tag>收 {round.received}</Tag>
                        </Space>
                    ),
                },
                {
                    title: '采集状态',
                    width: 110,
                    render: (_, round) => (
                        <Tag
                            color={
                                round.state === 'success'
                                    ? 'success'
                                    : round.state === 'failed'
                                      ? 'error'
                                      : round.state === 'partial'
                                        ? 'warning'
                                        : round.state === 'unreported'
                                          ? 'default'
                                          : 'processing'
                            }
                        >
                            {
                                {
                                    running: '采集中',
                                    unreported: '未上报轮次',
                                    success: '成功',
                                    partial: '部分失败',
                                    failed: '失败',
                                }[round.state]
                            }
                        </Tag>
                    ),
                },
                {
                    title: '历史保存',
                    width: 130,
                    render: (_, round) => (
                        <Tag color={round.storage_status === 'stored' ? 'success' : undefined}>
                            {{
                                stored: '已入库',
                                skipped:
                                    round.state === 'failed' || round.state === 'partial'
                                        ? '未保存'
                                        : '按策略不保存',
                                failed: '入库失败',
                                pending: '待入库',
                            }[round.storage_status ?? ''] ?? '暂无历史'}
                        </Tag>
                    ),
                },
                {
                    title: '采集轮次',
                    render: (_, round) => (
                        <Typography.Text
                            copyable={{ text: round.id }}
                            className="font-mono text-xs"
                        >
                            {round.id}
                        </Typography.Text>
                    ),
                },
            ]}
        />
    );
}

function AcquisitionDetails({ acquisition }: { acquisition: AcquisitionSummary }) {
    const parsed = acquisition.parsed_json ? acquisition : undefined;
    const positions = new Map(acquisition.packets.map((packet, index) => [packet.id, index + 1]));
    return (
        <div className="grid min-w-[880px] grid-cols-[minmax(560px,2fr)_minmax(280px,1fr)] gap-4 py-2">
            <div className="min-w-0">
                <Typography.Title level={5}>
                    收发明细 · {acquisition.packets.length} 条
                </Typography.Title>
                <Table<DebugPacket>
                    rowKey="id"
                    size="small"
                    dataSource={acquisition.packets}
                    pagination={{ pageSize: 10, showSizeChanger: false }}
                    scroll={{ x: 760, y: 340 }}
                    columns={[
                        { title: '#', width: 42, render: (_, packet) => positions.get(packet.id) },
                        {
                            title: '时间',
                            width: 228,
                            render: (_, packet) => (
                                <span className="whitespace-nowrap tabular-nums">
                                    {dayjs(Number(packet.time_ms)).format(
                                        'YYYY-MM-DD HH:mm:ss.SSS'
                                    )}
                                </span>
                            ),
                        },
                        {
                            title: '方向',
                            width: 72,
                            render: (_, packet) => (
                                <Tag color={packet.direction === 'TX' ? 'blue' : undefined}>
                                    {packet.direction === 'TX' || packet.direction === 'TX_ATTEMPT'
                                        ? '发送'
                                        : '接收'}
                                </Tag>
                            ),
                        },
                        {
                            title: '状态',
                            width: 120,
                            render: (_, packet) => <PacketStatuses packet={packet} />,
                        },
                        {
                            title: '原始报文 HEX',
                            width: 360,
                            render: (_, packet) => (
                                <>
                                    {packet.reply_to_packet_id && (
                                        <div className="mb-1 text-xs text-gray-500">
                                            {positions.has(packet.reply_to_packet_id)
                                                ? `关联第 ${positions.get(packet.reply_to_packet_id)} 条报文`
                                                : '关联报文不在当前明细中'}
                                        </div>
                                    )}
                                    <Typography.Paragraph
                                        className="mb-0 break-all font-mono text-xs"
                                        copyable={{ text: packet.payload_hex }}
                                    >
                                        {packet.payload_hex}
                                    </Typography.Paragraph>
                                </>
                            ),
                        },
                    ]}
                />
            </div>
            <div className="min-w-0">
                <Typography.Title level={5}>本轮解析结果</Typography.Title>
                <div className="max-h-[400px] overflow-auto rounded border border-solid border-gray-200 p-3">
                    {parsed ? (
                        <ParsedHistory packet={parsed} />
                    ) : (
                        <Empty
                            description="本轮暂无解析结果"
                            image={Empty.PRESENTED_IMAGE_SIMPLE}
                        />
                    )}
                </div>
            </div>
        </div>
    );
}

function PacketStatuses({ packet }: { packet: DebugPacket }) {
    const groups: [string | undefined, Record<string, string>][] = [
        [
            packet.transport_status,
            { sending: '发送中', sent: '已发送', received: '已接收', failed: '发送失败' },
        ],
        [packet.response_status, { waiting: '等待应答', success: '应答成功', failed: '应答失败' }],
        [packet.parse_status, { pending: '待解析', success: '解析成功', failed: '解析失败' }],
        [
            packet.storage_status,
            { pending: '待入库', stored: '已入库', skipped: '未保存', failed: '入库失败' },
        ],
    ];
    return (
        <Tooltip title={packet.reason || undefined}>
            <div className="flex flex-col items-start gap-1">
                {groups.map(([status, labels]) =>
                    status && labels[status] ? (
                        <Tag
                            key={labels[status]}
                            color={
                                status === 'failed'
                                    ? 'error'
                                    : status === 'stored' || status === 'success'
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
        values = parsed.values ?? parsed;
    } catch {
        return <Typography.Text type="secondary">解析数据格式无效</Typography.Text>;
    }
    return (
        <div className="space-y-1">
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
