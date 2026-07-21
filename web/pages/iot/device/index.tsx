/**
 * 设备管理页面 - 页面结构与视觉遵循 iot-manager 原版，当前仅接入北桥 CRUD。
 */

import {
    ApartmentOutlined,
    DeleteOutlined,
    EditOutlined,
    HistoryOutlined,
    PlusOutlined,
    ReloadOutlined,
    SendOutlined,
    ShareAltOutlined,
} from '@ant-design/icons';
import { App, Button, Card, Empty, Flex, Input, Result, Skeleton, Space, Tag, Tooltip } from 'antd';
import { type ReactNode, startTransition, useCallback, useEffect, useMemo, useState } from 'react';
import DeviceCard, { type DeviceCardItem } from '@/components/DeviceCard';
import { PageContainer } from '@/components/PageContainer';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { useLinkOptions } from '../link/link.service';
import DeviceFormModal, { type DeviceFormValues } from './DeviceFormModal';
import DeviceGroupPanel from './DeviceGroupPanel';
import {
    useDeviceDelete,
    useDeviceGroupTreeWithCount,
    useDeviceList,
    useDeviceSave,
} from './device.service';
import type { Device } from './device.types';
import type { DeviceGroup } from './device-group.types';

const { Search } = Input;
const EMPTY_DEVICE_LIST: Device.RealTimeData[] = [];
const DEVICE_CARD_GRID_CLASS = 'grid grid-cols-1 gap-3 xl:grid-cols-2 2xl:grid-cols-4';
const DEVICE_CARD_ACTION_BUTTON_CLASS =
    '!flex !h-8 !w-8 items-center justify-center !rounded-md text-slate-500 hover:!bg-slate-100 hover:!text-slate-900';
const DEVICE_CARD_DANGER_BUTTON_CLASS =
    '!flex !h-8 !w-8 items-center justify-center !rounded-md hover:!bg-red-50';

interface DeviceProtocolStats {
    total: number;
    online: number;
    offline: number;
    enabled: number;
}

interface DeviceStats extends DeviceProtocolStats {
    byProtocol: Record<string, DeviceProtocolStats>;
}

const createDeviceStats = (): DeviceStats => ({
    total: 0,
    online: 0,
    offline: 0,
    enabled: 0,
    byProtocol: {},
});

interface DeviceGroupStats extends DeviceProtocolStats {}

const createGroupStats = (): DeviceGroupStats => ({ total: 0, online: 0, offline: 0, enabled: 0 });

const isOnline = (device: Device.RealTimeData, now = Date.now()) => {
    if (device.reportTime) {
        const reportTime = new Date(device.reportTime).getTime();
        if (!Number.isNaN(reportTime)) {
            return now - reportTime < (device.online_timeout || 300) * 1000;
        }
    }
    return device.connectionState === 'online' || device.connected === true;
};

const accumulateStats = <T extends DeviceProtocolStats>(
    stats: T,
    device: Device.RealTimeData,
    now = Date.now()
) => {
    stats.total++;
    if (isOnline(device, now)) stats.online++;
    else stats.offline++;
    if (device.status === 'enabled') stats.enabled++;
};

const buildDeviceStats = (devices: Device.RealTimeData[], now = Date.now()) => {
    const stats = createDeviceStats();
    for (const device of devices) {
        accumulateStats(stats, device, now);
        const protocol = device.protocol_type || device.protocol_name || '未知';
        stats.byProtocol[protocol] ??= { total: 0, online: 0, offline: 0, enabled: 0 };
        accumulateStats(stats.byProtocol[protocol], device, now);
    }
    return stats;
};

const buildGroupIndex = (groups: DeviceGroup.TreeItem[]) => {
    const index = new Map<string, DeviceGroup.TreeItem>();
    const walk = (nodes: DeviceGroup.TreeItem[]) => {
        for (const node of nodes) {
            index.set(node.id, node);
            if (node.children?.length) walk(node.children);
        }
    };
    walk(groups);
    return index;
};

const buildGroupScopeIds = (group?: DeviceGroup.TreeItem) => {
    const scope = new Set<string>();
    const walk = (node: DeviceGroup.TreeItem) => {
        scope.add(node.id);
        node.children?.forEach(walk);
    };
    if (group) walk(group);
    return scope;
};

const buildGroupStats = (
    groups: DeviceGroup.TreeItem[],
    deviceMap: Map<string, Device.RealTimeData[]>,
    now = Date.now()
) => {
    const result = new Map<string, DeviceGroupStats>();
    const walk = (group: DeviceGroup.TreeItem): DeviceGroupStats => {
        const stats = createGroupStats();
        for (const device of deviceMap.get(group.id) ?? []) accumulateStats(stats, device, now);
        for (const child of group.children ?? []) {
            const childStats = walk(child);
            stats.total += childStats.total;
            stats.online += childStats.online;
            stats.offline += childStats.offline;
            stats.enabled += childStats.enabled;
        }
        result.set(group.id, stats);
        return stats;
    };
    groups.forEach(walk);
    return result;
};

const formatReportTime = (value?: string) => {
    if (!value) return '--';
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? value : date.toLocaleString();
};

const formatElementValue = (element: Device.Element) => {
    if (element.value === null || element.value === undefined || element.value === '') return '--';
    if (element.dictConfig?.mapType === 'VALUE') {
        return (
            element.dictConfig.items.find((item) => item.key === String(element.value))?.label ??
            String(element.value)
        );
    }
    const numeric = Number(element.value);
    const value =
        !Number.isNaN(numeric) && element.decimals !== undefined && element.decimals >= 0
            ? numeric.toFixed(element.decimals)
            : String(element.value);
    return element.unit ? `${value} ${element.unit}` : value;
};

const buildCardItems = (device: Device.RealTimeData): DeviceCardItem[] =>
    (device.elements ?? []).map((element, index) => ({
        key: index,
        label: element.name,
        children: formatElementValue(element),
        group: element.group,
    }));

const DevicePage = () => {
    const { modal, message } = App.useApp();
    const { has } = usePermissions();
    const canQuery = has('iot:device:query');
    const canAdd = has('iot:device:add');
    const canEdit = has('iot:device:edit');
    const canDelete = has('iot:device:delete');
    const canManageGroup =
        has('iot:device-group:add') ||
        has('iot:device-group:edit') ||
        has('iot:device-group:delete');

    const [keyword, setKeyword] = useState('');
    const [selectedGroupId, setSelectedGroupId] = useState<string | null>(null);
    const [formOpen, setFormOpen] = useState(false);
    const [editing, setEditing] = useState<Device.Item | null>(null);
    const [statusNow, setStatusNow] = useState(() => Date.now());

    useEffect(() => {
        const timer = window.setInterval(() => setStatusNow(Date.now()), 5000);
        return () => window.clearInterval(timer);
    }, []);

    const { data, isLoading, refetch } = useDeviceList({ enabled: canQuery });
    const { data: groupTree = [] } = useDeviceGroupTreeWithCount({ enabled: canQuery });
    const { data: linkOptions = [] } = useLinkOptions({ enabled: canQuery });
    const saveMutation = useDeviceSave();
    const deleteMutation = useDeviceDelete();
    const deviceList = data?.list ?? EMPTY_DEVICE_LIST;

    const groupIndex = useMemo(() => buildGroupIndex(groupTree), [groupTree]);
    const selectedGroup = useMemo(
        () =>
            selectedGroupId && selectedGroupId !== 'ungrouped'
                ? groupIndex.get(selectedGroupId)
                : undefined,
        [groupIndex, selectedGroupId]
    );
    const selectedScope = useMemo(() => buildGroupScopeIds(selectedGroup), [selectedGroup]);
    const ungroupedCount = useMemo(
        () => deviceList.filter((device) => !device.group_id).length,
        [deviceList]
    );
    const scopedDevices = useMemo(() => {
        if (selectedGroupId === null) return deviceList;
        if (selectedGroupId === 'ungrouped') return deviceList.filter((device) => !device.group_id);
        return deviceList.filter(
            (device) => !!device.group_id && selectedScope.has(device.group_id)
        );
    }, [deviceList, selectedGroupId, selectedScope]);
    const normalizedKeyword = keyword.trim().toLowerCase();
    const filteredDevices = useMemo(() => {
        if (!normalizedKeyword) return scopedDevices;
        return scopedDevices.filter((device) =>
            [device.name, device.device_code, device.protocol_name].some((value) =>
                value?.toLowerCase().includes(normalizedKeyword)
            )
        );
    }, [normalizedKeyword, scopedDevices]);
    const stats = useMemo(
        () => buildDeviceStats(scopedDevices, statusNow),
        [scopedDevices, statusNow]
    );
    const protocolStatsEntries = useMemo(
        () => Object.entries(stats.byProtocol),
        [stats.byProtocol]
    );
    const visibleDeviceMap = useMemo(() => {
        const map = new Map<string, Device.RealTimeData[]>();
        for (const device of filteredDevices) {
            if (!device.group_id) continue;
            const devices = map.get(device.group_id) ?? [];
            devices.push(device);
            map.set(device.group_id, devices);
        }
        return map;
    }, [filteredDevices]);
    const groupStats = useMemo(
        () => buildGroupStats(groupTree, visibleDeviceMap, statusNow),
        [groupTree, statusNow, visibleDeviceMap]
    );
    const groupRoots = useMemo(() => {
        if (selectedGroupId === 'ungrouped') return [];
        if (selectedGroupId !== null) return selectedGroup ? [selectedGroup] : [];
        return groupTree;
    }, [groupTree, selectedGroup, selectedGroupId]);
    const ungroupedDevices = useMemo(
        () => filteredDevices.filter((device) => !device.group_id),
        [filteredDevices]
    );

    const { run: debouncedSearch } = useDebounceFn((value: string) => {
        startTransition(() => setKeyword(value));
    }, 300);

    const openCreate = () => {
        setEditing(null);
        setFormOpen(true);
    };
    const openEdit = useCallback((device: Device.RealTimeData) => {
        setEditing(device);
        setFormOpen(true);
    }, []);
    const closeForm = () => {
        setFormOpen(false);
        setEditing(null);
    };
    const save = (values: DeviceFormValues) => {
        const {
            connection_mode: _connectionMode,
            agent_id: _agentId,
            agent_endpoint_id: _agentEndpointId,
            ...dto
        } = values;
        saveMutation.mutate({ ...dto, id: editing?.id }, { onSuccess: closeForm });
    };
    const remove = useCallback(
        (device: Device.RealTimeData) => {
            modal.confirm({
                title: `确认删除设备「${device.name}」吗？`,
                content: '删除后设备将停止数据采集，历史数据仍会保留。此操作不可撤销。',
                okText: '确定删除',
                okButtonProps: { danger: true },
                onOk: () => deleteMutation.mutateAsync(device.id),
            });
        },
        [deleteMutation, modal]
    );
    const unavailable = () => message.info('该功能将在南桥运行模块接入后启用');

    const renderDeviceCards = (devices: Device.RealTimeData[]) => (
        <div className={`mt-4 ${DEVICE_CARD_GRID_CLASS}`}>
            {devices.map((device) => {
                const online = isOnline(device, statusNow);
                const items = buildCardItems(device);
                const wide = (device.element_count ?? items.length) >= 18;
                return (
                    <div key={device.id} className={`flex flex-col ${wide ? 'xl:col-span-2' : ''}`}>
                        <DeviceCard
                            title={
                                <Flex
                                    justify="space-between"
                                    align="start"
                                    gap={10}
                                    className="w-full min-w-0"
                                >
                                    <span className="min-w-0 flex-1 whitespace-normal break-words pr-1 text-left leading-5">
                                        {device.name}:{device.device_code}
                                        <span className="ml-2 whitespace-nowrap text-xs font-normal text-slate-400">
                                            设备 ID：{device.id}
                                        </span>
                                    </span>
                                    <Tag
                                        color={online ? 'success' : 'error'}
                                        className="!mr-0 shrink-0 !rounded-md !px-2"
                                    >
                                        {online ? '在线' : '离线'}
                                    </Tag>
                                </Flex>
                            }
                            subtitle={
                                <div className="flex w-full min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
                                    <span className="flex min-w-0 shrink-0 items-center">
                                        <Tag color="blue" className="!mr-0 !rounded-md">
                                            {device.link_name || '未绑定链路'}
                                        </Tag>
                                        <Tag color="purple" className="!mr-0 !rounded-md">
                                            {device.protocol_name ||
                                                device.protocol_type ||
                                                '未配置协议'}
                                        </Tag>
                                    </span>
                                    <span className="min-w-0 truncate text-xs text-slate-400">
                                        上报：{formatReportTime(device.reportTime)}
                                    </span>
                                </div>
                            }
                            items={items}
                            column={wide ? 8 : 4}
                            extra={
                                <Flex
                                    align="center"
                                    justify="center"
                                    gap={10}
                                    wrap
                                    className="w-full"
                                >
                                    <Tooltip title="南桥接入后可下发指令">
                                        <Button
                                            type="text"
                                            size="small"
                                            className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                            icon={<SendOutlined />}
                                            disabled
                                        />
                                    </Tooltip>
                                    <Tooltip title="历史数据">
                                        <Button
                                            type="text"
                                            size="small"
                                            className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                            icon={<HistoryOutlined />}
                                            onClick={unavailable}
                                        />
                                    </Tooltip>
                                    {canEdit && (
                                        <Tooltip title="分享设备">
                                            <Button
                                                type="text"
                                                size="small"
                                                className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                                icon={<ShareAltOutlined />}
                                                onClick={unavailable}
                                            />
                                        </Tooltip>
                                    )}
                                    {canEdit && (
                                        <Tooltip title="编辑设备">
                                            <Button
                                                type="text"
                                                size="small"
                                                className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                                icon={<EditOutlined />}
                                                onClick={() => openEdit(device)}
                                            />
                                        </Tooltip>
                                    )}
                                    {canDelete && (
                                        <Tooltip title="删除设备">
                                            <Button
                                                type="text"
                                                danger
                                                size="small"
                                                className={DEVICE_CARD_DANGER_BUTTON_CLASS}
                                                icon={<DeleteOutlined />}
                                                onClick={() => remove(device)}
                                            />
                                        </Tooltip>
                                    )}
                                </Flex>
                            }
                        />
                    </div>
                );
            })}
        </div>
    );

    const renderSectionStats = (sectionStats: DeviceGroupStats) => (
        <Space size={6} wrap>
            <Tag color="blue">{sectionStats.total} 个</Tag>
            {sectionStats.online > 0 && <Tag color="green">{sectionStats.online} 在线</Tag>}
            {sectionStats.offline > 0 && <Tag color="red">{sectionStats.offline} 离线</Tag>}
            {sectionStats.enabled > 0 && <Tag color="purple">{sectionStats.enabled} 已启用</Tag>}
        </Space>
    );

    const renderGroupSection = (group: DeviceGroup.TreeItem, depth = 0): ReactNode => {
        const sectionStats = groupStats.get(group.id);
        if (!sectionStats?.total) return null;
        const directDevices = visibleDeviceMap.get(group.id) ?? [];
        const visibleChildren = (group.children ?? []).filter(
            (child) => (groupStats.get(child.id)?.total ?? 0) > 0
        );
        return (
            <section
                key={group.id}
                className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4"
                style={depth > 0 ? { marginLeft: depth * 16 } : undefined}
            >
                <Flex justify="space-between" align="center" gap={12} wrap>
                    <div className="text-sm font-semibold text-slate-800">{group.name}</div>
                    {renderSectionStats(sectionStats)}
                </Flex>
                {directDevices.length > 0 && renderDeviceCards(directDevices)}
                {visibleChildren.length > 0 && (
                    <Space direction="vertical" className="mt-4 w-full" size="middle">
                        {visibleChildren.map((child) => renderGroupSection(child, depth + 1))}
                    </Space>
                )}
            </section>
        );
    };

    const renderUngroupedSection = (devices: Device.RealTimeData[]) => {
        if (!devices.length) return null;
        return (
            <section className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4">
                <Flex justify="space-between" align="center" gap={12} wrap>
                    <div className="min-w-0">
                        <div className="text-sm font-semibold text-slate-800">未分组</div>
                        <div className="mt-1 text-xs text-slate-500">
                            没有绑定设备分组的卡片会统一在这里展示
                        </div>
                    </div>
                    {renderSectionStats(buildDeviceStats(devices, statusNow))}
                </Flex>
                {renderDeviceCards(devices)}
            </section>
        );
    };

    if (!canQuery) {
        return (
            <PageContainer>
                <Result
                    status="403"
                    title="无权限"
                    subTitle="您没有查询设备列表的权限，请联系管理员"
                />
            </PageContainer>
        );
    }

    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-center justify-between gap-2">
                    <h3 className="m-0 text-base font-medium">设备管理</h3>
                    <Space wrap>
                        <DeviceGroupPanel
                            selectedGroupId={selectedGroupId}
                            onSelect={setSelectedGroupId}
                            canManageGroup={canManageGroup}
                            ungroupedCount={ungroupedCount}
                        />
                        <Search
                            allowClear
                            placeholder="设备名称 / 编码 / 类型"
                            onChange={(event) => debouncedSearch(event.target.value)}
                            className="w-60"
                        />
                        <Tooltip title="拓扑视图">
                            <Button icon={<ApartmentOutlined />} onClick={unavailable} />
                        </Tooltip>
                        <Tooltip title="刷新">
                            <Button
                                icon={<ReloadOutlined />}
                                onClick={() => refetch()}
                                loading={isLoading}
                            />
                        </Tooltip>
                        {canAdd && (
                            <Button type="primary" icon={<PlusOutlined />} onClick={openCreate}>
                                新建设备
                            </Button>
                        )}
                    </Space>
                </div>
            }
        >
            <div className="h-full overflow-y-auto overflow-x-hidden">
                <Flex gap={12} className="mb-3" wrap>
                    {[
                        {
                            label: '设备总数',
                            value: stats.total,
                            color: '#1677ff',
                            tag: 'blue',
                            field: 'total',
                        },
                        {
                            label: '在线设备',
                            value: stats.online,
                            color: '#52c41a',
                            tag: 'green',
                            field: 'online',
                        },
                        {
                            label: '离线设备',
                            value: stats.offline,
                            color: '#ff4d4f',
                            tag: 'red',
                            field: 'offline',
                        },
                        {
                            label: '已启用',
                            value: stats.enabled,
                            color: '#722ed1',
                            tag: 'purple',
                            field: 'enabled',
                        },
                    ].map((item) => (
                        <Card
                            key={item.label}
                            size="small"
                            className="min-w-[140px] flex-1"
                            styles={{ body: { padding: '12px 16px' } }}
                        >
                            <Flex justify="space-between" align="center" className="mb-2.5">
                                <span className="text-[13px] text-gray-500">{item.label}</span>
                                <span
                                    className="text-lg font-semibold"
                                    style={{ color: item.color }}
                                >
                                    {item.value}
                                    {item.field !== 'total' && (
                                        <span className="text-[13px] font-normal text-gray-400">
                                            {' '}
                                            / {stats.total}
                                        </span>
                                    )}
                                </span>
                            </Flex>
                            <Flex gap={6} wrap>
                                {protocolStatsEntries.map(([protocol, protocolStats]) => (
                                    <Tag
                                        key={protocol}
                                        color={item.tag}
                                        className="!m-0 !px-3 !py-1 !text-sm !leading-5"
                                    >
                                        {protocol}:{' '}
                                        {protocolStats[item.field as keyof DeviceProtocolStats]}
                                        {item.field !== 'total' && `/${protocolStats.total}`}
                                    </Tag>
                                ))}
                            </Flex>
                        </Card>
                    ))}
                </Flex>

                {isLoading && filteredDevices.length === 0 ? (
                    <div className={DEVICE_CARD_GRID_CLASS}>
                        {['first', 'second', 'third', 'fourth'].map((key) => (
                            <div key={key} className="rounded-lg bg-white px-3.5 py-3">
                                <Skeleton active title paragraph={{ rows: 4 }} />
                            </div>
                        ))}
                    </div>
                ) : filteredDevices.length === 0 ? (
                    <div className="py-12">
                        <Empty
                            description={keyword ? '搜索无结果，请尝试调整关键词' : '暂无设备数据'}
                        />
                    </div>
                ) : (
                    <Space direction="vertical" className="w-full" size="large">
                        {groupRoots.length > 0 ? (
                            <>
                                {groupRoots.map((group) => renderGroupSection(group))}
                                {selectedGroupId === null &&
                                    renderUngroupedSection(ungroupedDevices)}
                            </>
                        ) : selectedGroupId === 'ungrouped' ? (
                            (renderUngroupedSection(ungroupedDevices) ?? (
                                <div className="py-12">
                                    <Empty description="暂无未分组设备" />
                                </div>
                            ))
                        ) : (
                            <section className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4">
                                <Flex justify="space-between" align="center" gap={12} wrap>
                                    <div className="text-sm font-semibold text-slate-800">
                                        {selectedGroup?.name ?? '全部设备'}
                                    </div>
                                    {renderSectionStats(
                                        buildDeviceStats(filteredDevices, statusNow)
                                    )}
                                </Flex>
                                {renderDeviceCards(filteredDevices)}
                            </section>
                        )}
                    </Space>
                )}
            </div>

            <DeviceFormModal
                open={formOpen}
                editing={editing}
                loading={saveMutation.isPending}
                linkOptions={linkOptions}
                onCancel={closeForm}
                onFinish={save}
            />
        </PageContainer>
    );
};

export default DevicePage;
