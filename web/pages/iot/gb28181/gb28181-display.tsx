import { Badge, Tag } from 'antd';
import type { GB28181 } from './gb28181.types';

export const onlineTag = (online: boolean) => (
    <Badge status={online ? 'processing' : 'default'} text={online ? '在线' : '离线'} />
);

export const displayText = (value?: string | number | null) => {
    if (value === undefined || value === null || value === '') return '--';
    return value;
};

export const remoteEndpoint = (device?: GB28181.Device) => {
    if (!device) return '--';
    const ip = device.remote_ip || device.remote_address;
    const port = device.remote_port;
    if (!ip) return '--';
    return port ? `${ip}:${port}` : ip;
};

export const registrationSourceTag = (source?: string) => {
    if (source === 'mock') {
        return <Tag color="orange">模拟</Tag>;
    }
    return null;
};

export const ptzCapabilityTag = (channel?: GB28181.Channel) => {
    if (!channel || channel.ptz_type === undefined || channel.ptz_type < 0) {
        return <Tag>云台未知</Tag>;
    }
    return channel.ptz_capable ? <Tag color="green">支持云台</Tag> : <Tag color="red">无云台</Tag>;
};
