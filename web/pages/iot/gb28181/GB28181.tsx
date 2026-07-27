import {
    PauseCircleOutlined,
    PlayCircleOutlined,
    ReloadOutlined,
    SendOutlined,
    VideoCameraOutlined,
} from '@ant-design/icons';
import { App, Button, Card, Input, Result, Select, Space, Tag, Typography } from 'antd';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { PageContainer } from '@/components/PageContainer';
import { usePermission } from '@/hooks/usePermission';
import {
    sendPtz,
    sendPtzPosition,
    stopPreviewKeepalive,
    useGb28181CatalogQuery,
    useGb28181Devices,
    useGb28181Health,
    useGb28181PreviewStart,
    useGb28181PreviewStop,
} from './gb28181.service';
import { useAuthStore } from '@/store/authStore';
import type { GB28181 } from './gb28181.types';
import { DeviceListCard } from './components/DeviceListCard';
import { Gb28181LivePlayer } from './components/LivePlayer';
import { PtzPanel } from './components/PtzPanel';
import { SessionDetailsCard } from './components/SessionDetailsCard';
import { ptzCapabilityTag } from './view';

const { Text, Title } = Typography;

export default function Gb28181Page() {
    const { message } = App.useApp();
    const canQuery = usePermission('iot:gb28181:query');
    const canControl = usePermission('iot:gb28181:control');
    const token = useAuthStore((state) => state.token);

    const [keyword, setKeyword] = useState('');
    const [selectedDeviceId, setSelectedDeviceId] = useState<string>();
    const [selectedChannelId, setSelectedChannelId] = useState<string>();
    const [activeSession, setActiveSession] = useState<GB28181.PreviewStartResult | null>(null);
    const [ptzSpeed, setPtzSpeed] = useState(80);
    const activeSessionRef = useRef<GB28181.PreviewStartResult | null>(null);
    const ptzSessionRef = useRef<GB28181.PreviewStartResult | null>(null);
    const ptzSpeedRef = useRef(ptzSpeed);
    const tokenRef = useRef<string | null>(token);

    const healthQuery = useGb28181Health({ enabled: canQuery });
    const devicesQuery = useGb28181Devices({ enabled: canQuery });

    const catalogMutation = useGb28181CatalogQuery();
    const previewStartMutation = useGb28181PreviewStart();
    const previewStopMutation = useGb28181PreviewStop();
    const devices = devicesQuery.data?.items ?? [];

    useEffect(() => {
        tokenRef.current = token;
    }, [token]);

    useEffect(() => {
        ptzSpeedRef.current = ptzSpeed;
    }, [ptzSpeed]);

    const releaseActiveSession = useCallback(() => {
        const session = activeSessionRef.current;
        if (!session) return;
        const ptzSession = ptzSessionRef.current;
        ptzSessionRef.current = null;
        if (ptzSession) {
            void sendPtz({
                deviceId: ptzSession.device_id,
                channelId: ptzSession.channel_id,
                action: 'stop',
                speed: ptzSpeedRef.current,
            }).catch(() => undefined);
        }
        activeSessionRef.current = null;
        stopPreviewKeepalive(session.session_id, tokenRef.current);
    }, []);

    useEffect(() => {
        activeSessionRef.current = activeSession;
    }, [activeSession]);

    useEffect(() => {
        const handlePageHide = () => releaseActiveSession();
        window.addEventListener('pagehide', handlePageHide);
        window.addEventListener('beforeunload', handlePageHide);
        return () => {
            window.removeEventListener('pagehide', handlePageHide);
            window.removeEventListener('beforeunload', handlePageHide);
            releaseActiveSession();
        };
    }, [releaseActiveSession]);

    const filteredDevices = useMemo(() => {
        const text = keyword.trim().toLowerCase();
        if (!text) return devices;
        return devices.filter((device) =>
            [
                device.id,
                device.name,
                device.manufacturer,
                device.remote_address,
                device.remote_ip,
                device.remote_port,
            ]
                .filter(Boolean)
                .some((item) => item.toLowerCase().includes(text))
        );
    }, [devices, keyword]);

    const selectedDevice = useMemo(
        () => devices.find((device) => device.id === selectedDeviceId) ?? devices[0],
        [devices, selectedDeviceId]
    );

    const channels = useMemo<GB28181.Channel[]>(() => {
        if (!selectedDevice) return [];
        if (selectedDevice.channels.length > 0) return selectedDevice.channels;
        return [
            {
                id: selectedDevice.id,
                name: selectedDevice.name || selectedDevice.id,
                manufacturer: selectedDevice.manufacturer,
                online: selectedDevice.online,
                ptz_type: -1,
                ptz_capable: false,
            },
        ];
    }, [selectedDevice]);

    const selectedChannel = useMemo(
        () => channels.find((channel) => channel.id === selectedChannelId) ?? channels[0],
        [channels, selectedChannelId]
    );

    const stats = useMemo(() => {
        const onlineDevices = devices.filter((device) => device.online).length;
        const channelCount = devices.reduce((sum, device) => sum + device.channels.length, 0);
        const onlineChannels = devices.reduce(
            (sum, device) => sum + device.channels.filter((channel) => channel.online).length,
            0
        );
        return { onlineDevices, channelCount, onlineChannels };
    }, [devices]);

    const activePtzDevice = activeSession
        ? devices.find((device) => device.id === activeSession.device_id)
        : undefined;
    const activePtzChannel = activePtzDevice?.channels.find(
        (channel) => channel.id === activeSession?.channel_id
    );
    const activeChannelSupportsPtz =
        activePtzChannel?.ptz_type === undefined ||
        activePtzChannel.ptz_type < 0 ||
        activePtzChannel.ptz_capable;
    const ptzDisabled =
        !canControl ||
        !activeSession ||
        !activePtzDevice?.online ||
        activePtzChannel?.online === false ||
        !activeChannelSupportsPtz;

    const channelOptions = channels.map((channel) => ({
        label: `${channel.name || channel.id} (${channel.id})`,
        value: channel.id,
    }));

    const refreshAll = () => {
        healthQuery.refetch();
        devicesQuery.refetch();
    };

    const currentTarget = () => {
        if (!selectedDevice || !selectedChannel) {
            message.warning('请选择在线设备和通道');
            return null;
        }
        return { deviceId: selectedDevice.id, channelId: selectedChannel.id };
    };

    const handleQueryCatalog = () => {
        if (!selectedDevice) {
            message.warning('请选择设备');
            return;
        }
        catalogMutation.mutate(selectedDevice.id, {
            onSuccess: () => {
                window.setTimeout(() => devicesQuery.refetch(), 1200);
            },
        });
    };

    const handleStartPreview = () => {
        const target = currentTarget();
        if (!target) return;
        previewStartMutation.mutate(target, {
            onSuccess: (result) => {
                const previousSession = activeSessionRef.current;
                if (previousSession && previousSession.session_id !== result.session_id) {
                    stopPreviewKeepalive(previousSession.session_id, tokenRef.current);
                }
                activeSessionRef.current = result;
                setActiveSession(result);
            },
        });
    };

    const handleStopSession = () => {
        if (!activeSession) {
            message.warning('当前没有活动会话');
            return;
        }
        previewStopMutation.mutate(
            { sessionId: activeSession.session_id },
            {
                onSuccess: () => {
                    activeSessionRef.current = null;
                    setActiveSession(null);
                },
            }
        );
    };

    const handlePtz = async (action: GB28181.PtzAction) => {
        const session =
            action === 'stop'
                ? (ptzSessionRef.current ?? activeSessionRef.current)
                : activeSessionRef.current;
        if (action === 'stop') {
            ptzSessionRef.current = null;
        } else {
            ptzSessionRef.current = session;
        }
        if (!session) {
            if (action !== 'stop') message.warning('请先开始实时预览');
            return;
        }

        try {
            await sendPtz({
                deviceId: session.device_id,
                channelId: session.channel_id,
                action,
                speed: ptzSpeed,
            });
        } catch (error) {
            if (action !== 'stop') {
                message.error(error instanceof Error ? error.message : '云台指令发送失败');
            }
            throw error;
        }
    };

    const handlePtzPosition = async (
        position: Pick<GB28181.PtzPositionPayload, 'pan' | 'tilt' | 'zoom'>
    ) => {
        const session = activeSessionRef.current;
        if (!session) {
            message.warning('请先开始实时预览');
            return;
        }

        try {
            await sendPtzPosition({
                deviceId: session.device_id,
                channelId: session.channel_id,
                ...position,
            });
            message.success('云台绝对定位指令已发送');
        } catch (error) {
            message.error(error instanceof Error ? error.message : '云台绝对定位指令发送失败');
            throw error;
        }
    };

    if (!canQuery) {
        return (
            <PageContainer>
                <Result
                    status="403"
                    title="无权限"
                    subTitle="您没有 GB28181 查询权限，请联系管理员。"
                />
            </PageContainer>
        );
    }

    const pageHeader = (
        <div className="flex flex-wrap items-center justify-between gap-3">
            <Space size={12} wrap>
                <Title level={4} className="!mb-0">
                    视频监控
                </Title>
                {healthQuery.data?.status === 'disabled' && <Tag>模块未启用</Tag>}
                {(healthQuery.isError || healthQuery.data?.status === 'error') && (
                    <Tag color="error">模块异常</Tag>
                )}
            </Space>
            <Space wrap>
                <Input.Search
                    allowClear
                    placeholder="搜索设备、编号、厂商、地址"
                    value={keyword}
                    onChange={(event) => setKeyword(event.target.value)}
                    className="w-[280px]"
                />
                <Button
                    icon={<ReloadOutlined />}
                    loading={devicesQuery.isFetching}
                    onClick={refreshAll}
                >
                    刷新
                </Button>
            </Space>
        </div>
    );

    return (
        <PageContainer header={pageHeader}>
            <div className="grid h-full grid-cols-1 gap-4 overflow-auto 2xl:grid-cols-[420px_minmax(0,1fr)]">
                <div className="space-y-4 min-w-0">
                    <DeviceListCard
                        devices={devices}
                        filteredDevices={filteredDevices}
                        selectedDevice={selectedDevice}
                        stats={stats}
                        loading={devicesQuery.isLoading}
                        onSelect={(device) => {
                            setSelectedDeviceId(device.id);
                            setSelectedChannelId(undefined);
                        }}
                    />
                </div>

                <div className="space-y-4 min-w-0">
                    <Card
                        size="small"
                        title={
                            <Space wrap>
                                <PlayCircleOutlined />
                                实时预览
                                {selectedDevice && (
                                    <Tag>{selectedDevice.name || selectedDevice.id}</Tag>
                                )}
                                {ptzCapabilityTag(selectedChannel)}
                                <Select
                                    size="small"
                                    value={selectedChannel?.id}
                                    placeholder="选择通道"
                                    options={channelOptions}
                                    disabled={!selectedDevice}
                                    className="min-w-[260px]"
                                    onChange={setSelectedChannelId}
                                />
                            </Space>
                        }
                        extra={
                            <Space wrap>
                                <Button
                                    icon={<SendOutlined />}
                                    disabled={!selectedDevice || !canControl}
                                    loading={catalogMutation.isPending}
                                    onClick={handleQueryCatalog}
                                >
                                    目录查询
                                </Button>
                                <Button
                                    type="primary"
                                    icon={<PlayCircleOutlined />}
                                    disabled={!canControl}
                                    loading={previewStartMutation.isPending}
                                    onClick={handleStartPreview}
                                >
                                    开始预览
                                </Button>
                                <Button
                                    danger
                                    icon={<PauseCircleOutlined />}
                                    disabled={!activeSession || !canControl}
                                    loading={previewStopMutation.isPending}
                                    onClick={handleStopSession}
                                >
                                    停止
                                </Button>
                            </Space>
                        }
                    >
                        <div className="relative aspect-video overflow-hidden rounded bg-black">
                            {activeSession ? (
                                <Gb28181LivePlayer
                                    key={activeSession.session_id}
                                    session={activeSession}
                                />
                            ) : (
                                <div className="flex h-full flex-col items-center justify-center text-white/70">
                                    <VideoCameraOutlined className="mb-3 text-5xl" />
                                    <Text className="!text-white/80">
                                        {selectedDevice
                                            ? `${selectedDevice.name || selectedDevice.id} / ${selectedChannel?.name || selectedChannel?.id || '未选择通道'}`
                                            : '请选择设备和通道'}
                                    </Text>
                                </div>
                            )}

                            {activeSession && activeChannelSupportsPtz ? (
                                <div className="absolute right-3 top-3 z-20">
                                    <PtzPanel
                                        speed={ptzSpeed}
                                        disabled={ptzDisabled}
                                        onSpeedChange={setPtzSpeed}
                                        onAction={handlePtz}
                                        onPosition={handlePtzPosition}
                                    />
                                </div>
                            ) : null}
                        </div>
                    </Card>

                    <SessionDetailsCard
                        selectedDevice={selectedDevice}
                        selectedChannel={selectedChannel}
                        activeSession={activeSession}
                    />
                </div>
            </div>
        </PageContainer>
    );
}
