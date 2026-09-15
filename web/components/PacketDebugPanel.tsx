import { Alert, Button, Empty, Space, Table, Tag, Typography } from 'antd';
import { useEffect, useRef, useState } from 'react';
import type { DebugPacket } from '@/types/packet_debug';
import { FormModal } from './FormModal';

interface Props {
    title: string;
    enabled: boolean;
    inherited?: boolean;
    open: boolean;
    pending: boolean;
    loading: boolean;
    error?: Error | null;
    packets?: DebugPacket[];
    onToggle: () => void;
    onOpen: () => void;
    onClose: () => void;
}
export function PacketDebugPanel(props: Props) {
    const tableHost = useRef<HTMLDivElement>(null);
    const [tableHeight, setTableHeight] = useState(200);
    useEffect(() => {
        const host = tableHost.current;
        if (!props.open || !host) return;
        const measure = () => setTableHeight(Math.max(80, host.clientHeight - 90));
        const observer = new ResizeObserver(measure);
        observer.observe(host);
        measure();
        return () => observer.disconnect();
    }, [props.open]);
    return (
        <>
            <Space size={2}>
                <Button size="small" type="text" loading={props.pending} onClick={props.onToggle}>
                    {props.enabled ? '关闭调试' : '开启调试'}
                </Button>
                <Button size="small" type="text" onClick={props.onOpen}>
                    报文
                </Button>
                {props.inherited && <Tag color="blue">链路调试中</Tag>}
            </Space>
            <FormModal
                title={props.title}
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
                        description="最近 500 条报文，实时更新。关闭窗口不会关闭调试；无新报文的列表保留 24 小时。"
                    />
                    {props.error && (
                        <Alert
                            type="error"
                            title="报文读取失败"
                            description={props.error.message}
                        />
                    )}
                    <div ref={tableHost} className="min-h-0 flex-1">
                        <Table<DebugPacket>
                            size="small"
                            rowKey="id"
                            loading={props.loading}
                            dataSource={props.packets}
                            pagination={{ pageSize: 20, showSizeChanger: false }}
                            scroll={{ x: 570, y: tableHeight }}
                            locale={{
                                emptyText: (
                                    <Empty description="暂无调试报文；边缘节点需支持调试并连接平台" />
                                ),
                            }}
                            columns={[
                                {
                                    title: '时间 / 方向',
                                    width: 160,
                                    onCell: () => ({ style: { verticalAlign: 'top' } }),
                                    render: (_, row) => (
                                        <>
                                            <div>
                                                {new Date(Number(row.time_ms)).toLocaleTimeString()}
                                            </div>
                                            <Tag>
                                                {row.direction === 'TX_ATTEMPT'
                                                    ? '尝试发送'
                                                    : row.direction === 'TX'
                                                      ? '发送'
                                                      : '接收'}
                                            </Tag>
                                            <div>
                                                {row.source === 'edge' ? '边缘节点' : '平台直采'}
                                            </div>
                                        </>
                                    ),
                                },
                                {
                                    title: '原始报文 HEX',
                                    render: (_, row) => (
                                        <>
                                            <div className="text-xs text-gray-500">
                                                {row.device_id || '未识别设备'} {row.address}
                                            </div>
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
                            ]}
                        />
                    </div>
                </div>
            </FormModal>
        </>
    );
}
