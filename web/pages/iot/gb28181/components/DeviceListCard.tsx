import { VideoCameraOutlined } from '@ant-design/icons';
import { Card, Space, Statistic, Table, Tag, Typography } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import type { GB28181 } from '../gb28181.types';
import { displayText, onlineTag, registrationSourceTag, remoteEndpoint } from '../view';

const { Text } = Typography;

type DeviceStats = {
    onlineDevices: number;
    channelCount: number;
    onlineChannels: number;
};

type DeviceListCardProps = {
    devices: GB28181.Device[];
    filteredDevices: GB28181.Device[];
    selectedDevice?: GB28181.Device;
    stats: DeviceStats;
    loading: boolean;
    onSelect: (device: GB28181.Device) => void;
};

export function DeviceListCard({
    devices,
    filteredDevices,
    selectedDevice,
    stats,
    loading,
    onSelect,
}: DeviceListCardProps) {
    const deviceColumns: ColumnsType<GB28181.Device> = [
        {
            title: '状态',
            dataIndex: 'online',
            width: 78,
            render: (online: boolean) => onlineTag(online),
        },
        {
            title: '设备',
            dataIndex: 'name',
            ellipsis: true,
            render: (name: string, record) => (
                <Space direction="vertical" size={2} className="min-w-0">
                    <Space size={4}>
                        <Text strong>{displayText(name)}</Text>
                        {registrationSourceTag(record.registration_source)}
                    </Space>
                    <Text type="secondary" className="text-xs">
                        {record.id}
                    </Text>
                    <Text type="secondary" className="text-xs">
                        IP {remoteEndpoint(record)}
                    </Text>
                </Space>
            ),
        },
        {
            title: '通道',
            width: 64,
            align: 'right',
            render: (_, record) => record.channels.length,
        },
    ];

    return (
        <Card
            size="small"
            title={
                <Space>
                    <VideoCameraOutlined />
                    设备列表
                </Space>
            }
            extra={
                <Tag color="blue">
                    {stats.onlineDevices}/{devices.length}
                </Tag>
            }
        >
            <div className="grid grid-cols-2 gap-3 mb-3">
                <Statistic
                    title="设备在线"
                    value={stats.onlineDevices}
                    suffix={`/ ${devices.length}`}
                    valueStyle={{ fontSize: 20 }}
                />
                <Statistic
                    title="通道在线"
                    value={stats.onlineChannels}
                    suffix={`/ ${stats.channelCount}`}
                    valueStyle={{ fontSize: 20 }}
                />
            </div>
            <Table
                rowKey="id"
                size="small"
                columns={deviceColumns}
                dataSource={filteredDevices}
                loading={loading}
                pagination={{ pageSize: 10, size: 'small' }}
                onRow={(record) => ({
                    onClick: () => onSelect(record),
                    className:
                        record.id === selectedDevice?.id
                            ? 'cursor-pointer bg-blue-50'
                            : 'cursor-pointer',
                })}
            />
        </Card>
    );
}
