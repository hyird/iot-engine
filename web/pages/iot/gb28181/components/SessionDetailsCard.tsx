import { Card, Descriptions } from 'antd';
import type { GB28181 } from '../gb28181.types';
import { displayText, onlineTag, ptzCapabilityTag, remoteEndpoint } from '../view';

type SessionDetailsCardProps = {
    selectedDevice?: GB28181.Device;
    selectedChannel?: GB28181.Channel;
    activeSession: GB28181.PreviewStartResult | null;
};

export function SessionDetailsCard({
    selectedDevice,
    selectedChannel,
    activeSession,
}: SessionDetailsCardProps) {
    return (
        <Card size="small" title="当前会话">
            <Descriptions size="small" column={{ xs: 1, lg: 2 }}>
                <Descriptions.Item label="设备">
                    {displayText(selectedDevice?.id)}
                </Descriptions.Item>
                <Descriptions.Item label="通道">
                    {displayText(selectedChannel?.id)}
                </Descriptions.Item>
                <Descriptions.Item label="云台">
                    {ptzCapabilityTag(selectedChannel)}
                </Descriptions.Item>
                <Descriptions.Item label="设备 IP">
                    {remoteEndpoint(selectedDevice)}
                </Descriptions.Item>
                <Descriptions.Item label="状态">
                    {selectedDevice ? onlineTag(selectedDevice.online) : '--'}
                </Descriptions.Item>
                <Descriptions.Item label="会话">
                    {displayText(activeSession?.session_id)}
                </Descriptions.Item>
                <Descriptions.Item label="RTP">
                    {displayText(activeSession?.rtp_port)}
                </Descriptions.Item>
            </Descriptions>
        </Card>
    );
}
