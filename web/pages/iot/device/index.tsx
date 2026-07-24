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
import { useVirtualizer } from '@tanstack/react-virtual';
import type { MenuProps } from 'antd';
import {
    App,
    Button,
    Card,
    DatePicker,
    Drawer,
    Dropdown,
    Empty,
    Flex,
    Form,
    Input,
    Pagination,
    Popconfirm,
    Popover,
    Result,
    Select,
    Skeleton,
    Space,
    Table,
    Tag,
    Tooltip,
    Typography,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import dayjs, { type Dayjs } from 'dayjs';
import {
    type CSSProperties,
    memo,
    type ReactNode,
    type RefObject,
    startTransition,
    useCallback,
    useEffect,
    useLayoutEffect,
    useMemo,
    useRef,
    useState,
} from 'react';
import DeviceCard, { type DeviceCardItem } from '@/components/DeviceCard';
import { PageContainer } from '@/components/PageContainer';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { formatDateTime } from '@/utils/dateTime';
import { useLinkOptions } from '../link/link.service';
import CommandPopover from './CommandPopover';
import DeviceFormModal, { type DeviceFormValues } from './DeviceFormModal';
import DeviceGroupPanel from './DeviceGroupPanel';
import {
    useDeviceDelete,
    useDeviceGroupShares,
    useDeviceGroupShareTargets,
    useDeviceGroupTreeWithCount,
    useDeviceHistory,
    useDeviceList,
    useDeviceSave,
    useDeviceShares,
    useDeviceShareTargets,
    useReplaceDeviceGroupShares,
    useReplaceDeviceShares,
} from './device.service';
import type { Device } from './device.types';
import type { DeviceGroup } from './device-group.types';

const { Search } = Input;
const EMPTY_DEVICE_LIST: Device.RealTimeData[] = [];
const EMPTY_COMMAND_OPERATIONS: Device.CommandOperation[] = [];
const DEVICE_CARD_GRID_CLASS = 'grid grid-cols-1 gap-3 xl:grid-cols-2 2xl:grid-cols-4';
const DEVICE_CARD_ACTION_BUTTON_CLASS =
    '!flex !h-8 !w-8 items-center justify-center !rounded-md text-slate-500 hover:!bg-slate-100 hover:!text-slate-900';
const DEVICE_CARD_DANGER_BUTTON_CLASS =
    '!flex !h-8 !w-8 items-center justify-center !rounded-md hover:!bg-red-50';
const WIDE_DEVICE_CARD_ITEM_COUNT = 18;
const DEVICE_VIRTUAL_ROW_GAP = 12;

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

const formatElementValue = (element: Device.Element) => {
    if (element.value === null || element.value === undefined || element.value === '') return '-';
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

const getDeviceDisplayElementCount = (device: Device.RealTimeData) =>
    device.element_count ?? device.elements?.length ?? 0;

const edgeNodeLabel = (device: Device.RealTimeData) =>
    device.edge_node_name || device.edge_node_imei || device.edge_node_id || '未绑定节点';

const edgeEndpointLabel = (device: Device.RealTimeData) => {
    if (device.edge_transport === 'serial') {
        const settings = [
            device.serial_baud_rate,
            device.serial_data_bits && device.serial_stop_bits
                ? `${device.serial_data_bits}N${device.serial_stop_bits}`
                : '',
        ]
            .filter(Boolean)
            .join(' · ');
        return `串口 ${device.edge_interface || '-'}${settings ? ` · ${settings}` : ''}`;
    }
    if (device.edge_transport === 'tcp') {
        const address =
            device.edge_ip && device.edge_port ? `${device.edge_ip}:${device.edge_port}` : '-';
        return `网口/TCP ${device.edge_mode || '-'} · ${address}`;
    }
    return '边缘链路未配置';
};

const tcpStateText = (device: Device.RealTimeData, online: boolean) => {
    const state = device.edgeTcpState?.trim();
    if (!state) return online ? 'TCP状态：在线' : 'TCP状态：待上报';
    const labelMap: Record<string, string> = {
        online: '在线',
        connected: '已连接',
        listening: '监听中',
        connecting: '连接中',
        offline: '离线',
        error: '异常',
        failed: '异常',
    };
    const label = labelMap[state.toLowerCase()] ?? state;
    const clients =
        device.edgeTcpClientCount !== undefined ? ` · ${device.edgeTcpClientCount}连接` : '';
    return `TCP状态：${label}${clients}`;
};

const tcpStateColor = (device: Device.RealTimeData, online: boolean) => {
    const state = device.edgeTcpState?.toLowerCase();
    if (!state) return online ? 'green' : 'default';
    if (['online', 'connected', 'listening'].includes(state)) return 'green';
    if (['connecting'].includes(state)) return 'processing';
    if (['error', 'failed'].includes(state)) return 'red';
    return 'default';
};

const DEVICE_ACCESS_LEVEL_OPTIONS: Array<{
    value: Device.ShareAccessLevel;
    label: string;
}> = [
    { value: 'view', label: '只读：查看设备与遥测' },
    { value: 'operate', label: '操作：只读 + 下发命令' },
];

interface DeviceShareFormValues {
    department_ids?: string[];
    user_ids?: string[];
    access_level: Device.ShareAccessLevel;
}

interface DeviceShareDrawerProps {
    resource: { kind: 'device' | 'group'; id: string; name: string } | null;
    onClose: () => void;
}

const DeviceShareDrawer = ({ resource, onClose }: DeviceShareDrawerProps) => {
    const { message } = App.useApp();
    const [form] = Form.useForm<DeviceShareFormValues>();
    const open = !!resource;
    const isGroup = resource?.kind === 'group';
    const deviceId = resource?.kind === 'device' ? resource.id : undefined;
    const groupId = resource?.kind === 'group' ? resource.id : undefined;
    const deviceShares = useDeviceShares(deviceId, {
        enabled: open && !isGroup,
    });
    const deviceTargets = useDeviceShareTargets(deviceId, {
        enabled: open && !isGroup,
    });
    const groupShares = useDeviceGroupShares(groupId, { enabled: open && isGroup });
    const groupTargets = useDeviceGroupShareTargets(groupId, { enabled: open && isGroup });
    const replaceDeviceShares = useReplaceDeviceShares();
    const replaceGroupShares = useReplaceDeviceGroupShares();
    const shares = (isGroup ? groupShares.data : deviceShares.data) ?? [];
    const targets = (isGroup ? groupTargets.data : deviceTargets.data) ?? [];
    const sharesLoading = isGroup ? groupShares.isLoading : deviceShares.isLoading;
    const targetsLoading = isGroup ? groupTargets.isLoading : deviceTargets.isLoading;
    const replacing = replaceDeviceShares.isPending || replaceGroupShares.isPending;

    const departmentOptions = useMemo(
        () =>
            targets
                .filter((target) => target.subject_type === 'department')
                .map((target) => ({ label: target.subject_name, value: target.subject_id })),
        [targets]
    );
    const userOptions = useMemo(
        () =>
            targets
                .filter((target) => target.subject_type === 'user')
                .map((target) => ({ label: target.subject_name, value: target.subject_id })),
        [targets]
    );

    const replace = (nextShares: Device.ReplaceSharesDto['shares'], onSuccess?: () => void) => {
        if (!resource) return;
        if (resource.kind === 'group') {
            replaceGroupShares.mutate(
                { groupId: resource.id, data: { shares: nextShares } },
                { onSuccess }
            );
            return;
        }
        replaceDeviceShares.mutate(
            { deviceId: resource.id, data: { shares: nextShares } },
            { onSuccess }
        );
    };

    const addOrUpdate = async () => {
        const values = await form.validateFields();
        const departmentIds = values.department_ids ?? [];
        const userIds = values.user_ids ?? [];
        if (departmentIds.length === 0 && userIds.length === 0) {
            message.warning('请至少选择一个部门或用户');
            return;
        }
        const nextShares = new Map<string, Device.ReplaceSharesDto['shares'][number]>();
        for (const share of shares) {
            if (share.inherited) continue;
            nextShares.set(`${share.subject_type}:${share.subject_id}`, {
                subject_type: share.subject_type,
                subject_id: share.subject_id,
                access_level: share.access_level,
            });
        }
        for (const subjectId of departmentIds) {
            nextShares.set(`department:${subjectId}`, {
                subject_type: 'department',
                subject_id: subjectId,
                access_level: values.access_level,
            });
        }
        for (const subjectId of userIds) {
            nextShares.set(`user:${subjectId}`, {
                subject_type: 'user',
                subject_id: subjectId,
                access_level: values.access_level,
            });
        }
        replace([...nextShares.values()], () => {
            form.setFieldsValue({ department_ids: [], user_ids: [] });
        });
    };

    const removeShare = (share: Device.ShareItem) => {
        replace(
            shares
                .filter((item) => !item.inherited && item.id !== share.id)
                .map((item) => ({
                    subject_type: item.subject_type,
                    subject_id: item.subject_id,
                    access_level: item.access_level,
                }))
        );
    };

    const columns: ColumnsType<Device.ShareItem> = [
        {
            title: '类型',
            dataIndex: 'subject_type',
            width: 82,
            render: (value: Device.ShareItem['subject_type']) =>
                value === 'department' ? (
                    <Tag color="purple">部门</Tag>
                ) : (
                    <Tag color="blue">用户</Tag>
                ),
        },
        { title: '分享对象', dataIndex: 'subject_name', ellipsis: true },
        {
            title: '访问级别',
            dataIndex: 'access_level',
            width: 104,
            render: (value: Device.ShareAccessLevel) => {
                const option = DEVICE_ACCESS_LEVEL_OPTIONS.find((item) => item.value === value);
                const color = value === 'operate' ? 'orange' : 'blue';
                return <Tag color={color}>{option?.label.split('：')[0] ?? value}</Tag>;
            },
        },
        {
            title: '更新时间',
            dataIndex: 'updated_at',
            width: 176,
            render: (value?: string) => formatDateTime(value),
        },
        {
            title: '来源',
            key: 'source',
            width: 150,
            render: (_, share) =>
                share.inherited ? (
                    <Tooltip title={`只能在设备分组「${share.source_group_name}」中修改`}>
                        <Tag color="cyan">继承 · {share.source_group_name}</Tag>
                    </Tooltip>
                ) : (
                    <Tag>直接授权</Tag>
                ),
        },
        {
            title: '操作',
            key: 'actions',
            width: 76,
            render: (_, share) =>
                share.inherited ? (
                    <Typography.Text type="secondary">分组维护</Typography.Text>
                ) : (
                    <Popconfirm
                        title={`取消「${share.subject_name}」的分享？`}
                        okText="确认"
                        cancelText="取消"
                        onConfirm={() => removeShare(share)}
                    >
                        <Button type="link" danger icon={<DeleteOutlined />}>
                            移除
                        </Button>
                    </Popconfirm>
                ),
        },
    ];

    return (
        <Drawer
            open={open}
            title={
                resource
                    ? `${resource.kind === 'group' ? '设备分组分享' : '设备分享'}：${resource.name}`
                    : '分享'
            }
            placement="right"
            width={760}
            onClose={onClose}
            destroyOnHidden
            afterOpenChange={(nextOpen) => {
                if (!nextOpen) form.resetFields();
            }}
        >
            <Space direction="vertical" size="large" className="w-full">
                <Typography.Text type="secondary">
                    {isGroup
                        ? '权限覆盖本组及全部子分组，新增或移入的设备自动继承，移出后自动失效。分享对象只能查看设备或下发命令。'
                        : '分组继承权限在这里仅展示，必须回到来源分组修改；设备直接授权可独立新增、更新或移除，最终权限与继承权限取并集。'}
                </Typography.Text>
                <Form<DeviceShareFormValues>
                    form={form}
                    layout="vertical"
                    initialValues={{ department_ids: [], user_ids: [], access_level: 'view' }}
                >
                    <Form.Item label="部门" name="department_ids">
                        <Select
                            mode="multiple"
                            allowClear
                            showSearch
                            optionFilterProp="label"
                            placeholder="选择部门（可多选）"
                            options={departmentOptions}
                            loading={targetsLoading}
                        />
                    </Form.Item>
                    <Form.Item label="用户" name="user_ids">
                        <Select
                            mode="multiple"
                            allowClear
                            showSearch
                            optionFilterProp="label"
                            placeholder="选择用户（可多选）"
                            options={userOptions}
                            loading={targetsLoading}
                        />
                    </Form.Item>
                    <Form.Item label="访问级别" name="access_level">
                        <Select options={DEVICE_ACCESS_LEVEL_OPTIONS} />
                    </Form.Item>
                    <Form.Item className="!mb-0">
                        <Button type="primary" block loading={replacing} onClick={addOrUpdate}>
                            添加 / 更新分享
                        </Button>
                    </Form.Item>
                </Form>
                <Table<Device.ShareItem>
                    rowKey={(share) => `${share.source_type ?? 'device'}:${share.id}`}
                    columns={columns}
                    dataSource={shares}
                    loading={sharesLoading}
                    pagination={false}
                    size="middle"
                    scroll={{ x: 'max-content', y: 'calc(100dvh - 520px)' }}
                    locale={{
                        emptyText: isGroup ? '当前分组暂无分享记录' : '当前设备暂无分享记录',
                    }}
                />
            </Space>
        </Drawer>
    );
};

const createDefaultHistoryRange = (): [Dayjs, Dayjs] => [dayjs().subtract(24, 'hour'), dayjs()];

const formatHistoryValue = (value: unknown) => {
    if (value === null || value === undefined || value === '') return '-';
    if (typeof value === 'object') return JSON.stringify(value);
    return String(value);
};

const DeviceHistoryDrawer = ({
    device,
    onClose,
}: {
    device: Device.RealTimeData;
    onClose: () => void;
}) => {
    const [pagination, setPagination] = useState({ page: 1, pageSize: 20 });
    const [range, setRange] = useState<[Dayjs, Dayjs]>(createDefaultHistoryRange);

    const query = useMemo<Device.HistoryRecordQuery>(
        () => ({
            ...pagination,
            startTime: range[0].toISOString(),
            endTime: range[1].toISOString(),
        }),
        [pagination, range]
    );
    const { data, isLoading, isFetching } = useDeviceHistory(device.id, query);

    const columns = useMemo<ColumnsType<Device.HistoryRecord>>(
        () => [
            {
                title: '上报时间',
                dataIndex: 'reportTime',
                key: 'reportTime',
                width: 180,
                fixed: 'left',
                render: (value) => formatDateTime(value),
            },
            {
                title: '协议',
                dataIndex: 'protocol',
                key: 'protocol',
                width: 100,
                render: (value) => <Tag color="blue">{value}</Tag>,
            },
            {
                title: '功能码',
                dataIndex: 'functionCode',
                key: 'functionCode',
                width: 100,
                render: (value) => value || '-',
            },
            {
                title: '历史测点',
                dataIndex: 'values',
                key: 'values',
                width: 560,
                render: (values: Device.HistoryRecord['values']) => {
                    const entries = Object.entries(values ?? {});
                    if (entries.length === 0) return '-';
                    return (
                        <Flex gap={4} wrap>
                            {entries.map(([elementId, point]) => {
                                const text = `${point.name || elementId}: ${formatHistoryValue(point.value)}${point.unit ? ` ${point.unit}` : ''}`;
                                return (
                                    <Tooltip key={elementId} title={text}>
                                        <Tag className="!m-0 max-w-[260px] truncate">{text}</Tag>
                                    </Tooltip>
                                );
                            })}
                        </Flex>
                    );
                },
            },
            {
                title: '来源',
                dataIndex: 'source',
                key: 'source',
                width: 100,
                render: (value) => value || '-',
            },
        ],
        []
    );

    return (
        <Drawer
            open
            onClose={onClose}
            title={`历史数据 · ${device.name}:${device.device_code}`}
            width="min(1100px, 94vw)"
            destroyOnHidden
        >
            <div className="flex h-full min-h-0 flex-col">
                <Flex justify="space-between" align="center" gap={12} wrap className="mb-3">
                    <DatePicker.RangePicker
                        showTime
                        allowClear={false}
                        value={range}
                        onChange={(value) => {
                            if (!value?.[0] || !value[1]) return;
                            setRange([value[0], value[1]]);
                            setPagination((current) => ({ ...current, page: 1 }));
                        }}
                    />
                    <span className="text-xs text-slate-500">
                        直接查询数据库中已持久化的设备上报记录
                    </span>
                </Flex>
                <div className="min-h-0 flex-1">
                    <Table
                        rowKey="id"
                        size="small"
                        sticky
                        pagination={false}
                        columns={columns}
                        dataSource={data?.list ?? []}
                        loading={isLoading || isFetching}
                        scroll={{ x: 'max-content', y: 'calc(100dvh - 300px)' }}
                        locale={{ emptyText: <Empty description="当前时间范围暂无历史数据" /> }}
                    />
                </div>
                <Flex justify="flex-end" className="mt-3 shrink-0">
                    <Pagination
                        {...pagination}
                        total={data?.total ?? 0}
                        showSizeChanger
                        showTotal={(total) => `共 ${total} 条`}
                        onChange={(page, pageSize) => setPagination({ page, pageSize })}
                    />
                </Flex>
            </div>
        </Drawer>
    );
};

interface DeviceGridItemProps {
    device: Device.RealTimeData;
    online: boolean;
    onHistory: (device: Device.RealTimeData) => void;
    onShare: (device: Device.RealTimeData) => void;
    onEdit: (device: Device.RealTimeData) => void;
    onRemove: (device: Device.RealTimeData) => void;
    commandPopoverOpen: boolean;
    commandDeviceId?: string;
    commandFunc: Device.CommandOperation | null;
    onOpenCommandPopover: (device: Device.RealTimeData, operation: Device.CommandOperation) => void;
    onCloseCommandPopover: () => void;
}

const DeviceGridItem = memo(
    ({
        device,
        online,
        onHistory,
        onShare,
        onEdit,
        onRemove,
        commandPopoverOpen,
        commandDeviceId,
        commandFunc,
        onOpenCommandPopover,
        onCloseCommandPopover,
    }: DeviceGridItemProps) => {
        const items = useMemo(() => buildCardItems(device), [device]);
        const wide = getDeviceDisplayElementCount(device) >= WIDE_DEVICE_CARD_ITEM_COUNT;
        const commandOperations = device.commandOperations ?? EMPTY_COMMAND_OPERATIONS;
        const commandMenuItems = useMemo<MenuProps['items']>(
            () =>
                commandOperations.map((operation, index) => ({
                    key: String(index),
                    label: operation.name,
                })),
            [commandOperations]
        );
        const canRemoteControl = device.remote_control !== false;
        const isCommandPopoverOpen = commandPopoverOpen && commandDeviceId === device.id;
        const isEdgeTcp = device.edge_node_id && device.edge_transport === 'tcp';

        return (
            <div className={`flex flex-col ${wide ? 'xl:col-span-2' : ''}`}>
                <DeviceCard
                    title={
                        <Flex
                            justify="space-between"
                            align="start"
                            gap={10}
                            className="w-full min-w-0"
                        >
                            <div className="min-w-0 flex-1 pr-1 text-left">
                                <div className="whitespace-normal break-words leading-5">
                                    {device.name}
                                </div>
                                <div className="mt-0.5 flex flex-wrap gap-x-2 gap-y-0.5 text-xs font-normal text-slate-400">
                                    <span>编码：{device.device_code}</span>
                                    <span>设备 ID：{device.id}</span>
                                </div>
                            </div>
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
                            {device.edge_node_id ? (
                                <>
                                    <Tag color="blue" className="!mr-0 !rounded-md">
                                        边缘：{edgeNodeLabel(device)}
                                    </Tag>
                                    <Tag color="cyan" className="!mr-0 !rounded-md">
                                        {edgeEndpointLabel(device)}
                                    </Tag>
                                    {isEdgeTcp && (
                                        <Tooltip title={device.edgeTcpReason || undefined}>
                                            <Tag
                                                color={tcpStateColor(device, online)}
                                                className="!mr-0 !rounded-md"
                                            >
                                                {tcpStateText(device, online)}
                                            </Tag>
                                        </Tooltip>
                                    )}
                                </>
                            ) : (
                                <Tag color="blue" className="!mr-0 !rounded-md">
                                    {device.link_name || '未绑定连接'}
                                </Tag>
                            )}
                            <Tag color="purple" className="!mr-0 !rounded-md">
                                {device.protocol_name || device.protocol_type || '未配置协议'}
                            </Tag>
                            <span className="min-w-0 truncate text-xs text-slate-400">
                                上报：{formatDateTime(device.reportTime)}
                            </span>
                        </div>
                    }
                    items={items}
                    column={wide ? 8 : 4}
                    extra={
                        <Flex align="center" justify="center" gap={10} wrap className="w-full">
                            <Popover
                                open={isCommandPopoverOpen}
                                trigger="click"
                                placement="bottomRight"
                                content={
                                    isCommandPopoverOpen && commandFunc ? (
                                        <CommandPopover
                                            device={device}
                                            func={commandFunc}
                                            onClose={onCloseCommandPopover}
                                        />
                                    ) : null
                                }
                                onOpenChange={(open) => {
                                    if (!open) onCloseCommandPopover();
                                }}
                            >
                                <Dropdown
                                    disabled={
                                        !device.can_command ||
                                        !commandOperations.length ||
                                        !canRemoteControl
                                    }
                                    menu={{
                                        items: commandMenuItems,
                                        onClick: ({ key }) => {
                                            const operation = commandOperations[Number(key)];
                                            if (operation && device.can_command)
                                                onOpenCommandPopover(device, operation);
                                        },
                                    }}
                                >
                                    <Tooltip
                                        title={
                                            !device.can_command
                                                ? '当前账号没有设备下发权限'
                                                : !canRemoteControl
                                                  ? '该设备已禁止远控'
                                                  : !commandOperations.length
                                                    ? '当前协议没有可下发要素'
                                                    : online
                                                      ? '下发指令'
                                                      : '设备离线（点击后将提示）'
                                        }
                                    >
                                        <Button
                                            type="text"
                                            size="small"
                                            className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                            icon={<SendOutlined />}
                                            disabled={
                                                !device.can_command ||
                                                !commandOperations.length ||
                                                !canRemoteControl
                                            }
                                        />
                                    </Tooltip>
                                </Dropdown>
                            </Popover>
                            <Tooltip title="历史数据">
                                <Button
                                    type="text"
                                    size="small"
                                    className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                    icon={<HistoryOutlined />}
                                    onClick={() => onHistory(device)}
                                />
                            </Tooltip>
                            {device.can_share && (
                                <Tooltip title="分享设备">
                                    <Button
                                        type="text"
                                        size="small"
                                        className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                        icon={<ShareAltOutlined />}
                                        onClick={() => onShare(device)}
                                    />
                                </Tooltip>
                            )}
                            {device.can_edit && (
                                <Tooltip title="编辑设备">
                                    <Button
                                        type="text"
                                        size="small"
                                        className={DEVICE_CARD_ACTION_BUTTON_CLASS}
                                        icon={<EditOutlined />}
                                        onClick={() => onEdit(device)}
                                    />
                                </Tooltip>
                            )}
                            {device.can_delete && (
                                <Tooltip title="删除设备">
                                    <Button
                                        type="text"
                                        danger
                                        size="small"
                                        className={DEVICE_CARD_DANGER_BUTTON_CLASS}
                                        icon={<DeleteOutlined />}
                                        onClick={() => onRemove(device)}
                                    />
                                </Tooltip>
                            )}
                        </Flex>
                    }
                />
            </div>
        );
    }
);

interface DeviceGridProps extends Omit<DeviceGridItemProps, 'device' | 'online'> {
    devices: Device.RealTimeData[];
    statusNow: number;
    scrollElementRef: RefObject<HTMLDivElement | null>;
}

const getDeviceColumnCount = () => {
    if (window.matchMedia('(min-width: 1536px)').matches) return 4;
    if (window.matchMedia('(min-width: 1280px)').matches) return 2;
    return 1;
};

const useResponsiveDeviceColumnCount = () => {
    const [columnCount, setColumnCount] = useState(getDeviceColumnCount);

    useEffect(() => {
        const updateColumnCount = () => setColumnCount(getDeviceColumnCount());
        const desktopQuery = window.matchMedia('(min-width: 1280px)');
        const wideQuery = window.matchMedia('(min-width: 1536px)');
        desktopQuery.addEventListener('change', updateColumnCount);
        wideQuery.addEventListener('change', updateColumnCount);
        return () => {
            desktopQuery.removeEventListener('change', updateColumnCount);
            wideQuery.removeEventListener('change', updateColumnCount);
        };
    }, []);

    return columnCount;
};

const buildDeviceRows = (devices: Device.RealTimeData[], columnCount: number) => {
    const rows: Device.RealTimeData[][] = [];
    let row: Device.RealTimeData[] = [];
    let occupiedColumns = 0;

    for (const device of devices) {
        const wide = getDeviceDisplayElementCount(device) >= WIDE_DEVICE_CARD_ITEM_COUNT;
        const span = wide && columnCount > 1 ? 2 : 1;
        if (row.length > 0 && occupiedColumns + span > columnCount) {
            rows.push(row);
            row = [];
            occupiedColumns = 0;
        }
        row.push(device);
        occupiedColumns += span;
        if (occupiedColumns >= columnCount) {
            rows.push(row);
            row = [];
            occupiedColumns = 0;
        }
    }
    if (row.length > 0) rows.push(row);
    return rows;
};

const DeviceGrid = memo(
    ({ devices, statusNow, scrollElementRef, ...itemProps }: DeviceGridProps) => {
        const columnCount = useResponsiveDeviceColumnCount();
        const rows = useMemo(() => buildDeviceRows(devices, columnCount), [columnCount, devices]);
        const rootRef = useRef<HTMLDivElement>(null);
        const [scrollMargin, setScrollMargin] = useState(0);

        const updateScrollMargin = useCallback(() => {
            const root = rootRef.current;
            const scrollElement = scrollElementRef.current;
            if (!root || !scrollElement) return;

            const rootRect = root.getBoundingClientRect();
            const scrollRect = scrollElement.getBoundingClientRect();
            const nextMargin = rootRect.top - scrollRect.top + scrollElement.scrollTop;
            setScrollMargin((currentMargin) =>
                Math.abs(currentMargin - nextMargin) < 0.5 ? currentMargin : nextMargin
            );
        }, [scrollElementRef]);

        useLayoutEffect(() => {
            const root = rootRef.current;
            const scrollElement = scrollElementRef.current;
            if (!root || !scrollElement) return;

            updateScrollMargin();
            const observer = new ResizeObserver(updateScrollMargin);
            observer.observe(scrollElement);
            let ancestor = root.parentElement;
            while (ancestor && ancestor !== scrollElement) {
                observer.observe(ancestor);
                ancestor = ancestor.parentElement;
            }
            window.addEventListener('resize', updateScrollMargin);
            return () => {
                observer.disconnect();
                window.removeEventListener('resize', updateScrollMargin);
            };
        }, [scrollElementRef, updateScrollMargin]);

        const virtualizer = useVirtualizer<HTMLDivElement, HTMLDivElement>({
            count: rows.length,
            getScrollElement: () => scrollElementRef.current,
            estimateSize: () => 340,
            getItemKey: (index) => rows[index].map((device) => device.id).join(':'),
            overscan: 3,
            scrollMargin,
        });

        if (devices.length === 0) return null;

        return (
            <div
                ref={rootRef}
                className="relative mt-4 w-full"
                style={{ height: virtualizer.getTotalSize() }}
            >
                {virtualizer.getVirtualItems().map((virtualRow) => (
                    <div
                        key={virtualRow.key}
                        ref={virtualizer.measureElement}
                        data-index={virtualRow.index}
                        className="absolute left-0 top-0 w-full"
                        style={{
                            transform: `translateY(${virtualRow.start - scrollMargin}px)`,
                            paddingBottom: DEVICE_VIRTUAL_ROW_GAP,
                        }}
                    >
                        <div className={DEVICE_CARD_GRID_CLASS}>
                            {rows[virtualRow.index].map((device) => (
                                <DeviceGridItem
                                    key={device.id}
                                    device={device}
                                    online={isOnline(device, statusNow)}
                                    {...itemProps}
                                />
                            ))}
                        </div>
                    </div>
                ))}
            </div>
        );
    }
);

const DevicePage = () => {
    const { modal, message } = App.useApp();
    const { has } = usePermissions();
    const canQuery = has('iot:device:query');
    const canAdd = has('iot:device:add');
    const canManageGroup =
        has('iot:device-group:add') ||
        has('iot:device-group:edit') ||
        has('iot:device-group:delete');

    const [keyword, setKeyword] = useState('');
    const [selectedGroupId, setSelectedGroupId] = useState<string | null>(null);
    const [formOpen, setFormOpen] = useState(false);
    const [editing, setEditing] = useState<Device.Item | null>(null);
    const [sharing, setSharing] = useState<{
        kind: 'device' | 'group';
        id: string;
        name: string;
    } | null>(null);
    const [historyDevice, setHistoryDevice] = useState<Device.RealTimeData | null>(null);
    const [commandPopoverOpen, setCommandPopoverOpen] = useState(false);
    const [commandDevice, setCommandDevice] = useState<Device.RealTimeData | null>(null);
    const [commandFunc, setCommandFunc] = useState<Device.CommandOperation | null>(null);
    const [statusNow, setStatusNow] = useState(() => Date.now());
    const scrollContainerRef = useRef<HTMLDivElement>(null);

    useEffect(() => {
        const timer = window.setInterval(() => setStatusNow(Date.now()), 5000);
        return () => window.clearInterval(timer);
    }, []);

    const { data, isLoading, refetch } = useDeviceList({ enabled: canQuery });
    const { data: groupTree = [] } = useDeviceGroupTreeWithCount({ enabled: canQuery });
    const { data: linkOptions = [] } = useLinkOptions({ enabled: canQuery });
    const saveMutation = useDeviceSave();
    const { mutateAsync: deleteDevice } = useDeviceDelete();
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
    const scrollScopeKey = `${selectedGroupId ?? 'all'}:${normalizedKeyword}`;
    const previousScrollScopeKeyRef = useRef(scrollScopeKey);
    useLayoutEffect(() => {
        if (previousScrollScopeKeyRef.current !== scrollScopeKey && scrollContainerRef.current) {
            scrollContainerRef.current.scrollTop = 0;
        }
        previousScrollScopeKeyRef.current = scrollScopeKey;
    }, [scrollScopeKey]);
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
    const openShare = useCallback((device: Device.RealTimeData) => {
        setSharing({ kind: 'device', id: device.id, name: device.name });
    }, []);
    const openHistory = useCallback((device: Device.RealTimeData) => {
        setHistoryDevice(device);
    }, []);
    const openGroupShare = useCallback((group: DeviceGroup.TreeItem) => {
        setSharing({ kind: 'group', id: group.id, name: group.name });
    }, []);
    const closeForm = () => {
        setFormOpen(false);
        setEditing(null);
    };
    const save = (values: DeviceFormValues) => {
        const { connection_mode: _connectionMode, edge_protocol: _edgeProtocol, ...dto } = values;
        saveMutation.mutate({ ...dto, id: editing?.id }, { onSuccess: closeForm });
    };
    const remove = useCallback(
        (device: Device.RealTimeData) => {
            modal.confirm({
                title: `确认删除设备「${device.name}」吗？`,
                content: '删除后设备将停止数据采集，历史数据仍会保留。此操作不可撤销。',
                okText: '确定删除',
                okButtonProps: { danger: true },
                onOk: () => deleteDevice(device.id),
            });
        },
        [deleteDevice, modal]
    );
    const unavailable = useCallback(() => message.info('拓扑视图暂未开放'), [message]);
    const openCommandPopover = useCallback(
        (device: Device.RealTimeData, operation: Device.CommandOperation) => {
            setCommandDevice(device);
            setCommandFunc(operation);
            setCommandPopoverOpen(true);
        },
        []
    );
    const closeCommandPopover = useCallback(() => {
        setCommandPopoverOpen(false);
        setCommandDevice(null);
        setCommandFunc(null);
    }, []);

    const renderDeviceCards = (devices: Device.RealTimeData[]) => (
        <DeviceGrid
            devices={devices}
            statusNow={statusNow}
            scrollElementRef={scrollContainerRef}
            onHistory={openHistory}
            onShare={openShare}
            onEdit={openEdit}
            onRemove={remove}
            commandPopoverOpen={commandPopoverOpen}
            commandDeviceId={commandDevice?.id}
            commandFunc={commandFunc}
            onOpenCommandPopover={openCommandPopover}
            onCloseCommandPopover={closeCommandPopover}
        />
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
                className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4 [margin-left:var(--group-depth-offset)]"
                style={
                    {
                        '--group-depth-offset': depth > 0 ? `${depth * 16}px` : '0px',
                    } as CSSProperties
                }
            >
                <Flex justify="space-between" align="center" gap={12} wrap>
                    <div className="text-sm font-semibold text-slate-800">{group.name}</div>
                    <Space size={6} wrap>
                        {renderSectionStats(sectionStats)}
                        {group.can_share && (
                            <Tooltip title="分享整个设备分组">
                                <Button
                                    type="text"
                                    size="small"
                                    icon={<ShareAltOutlined />}
                                    onClick={() => openGroupShare(group)}
                                />
                            </Tooltip>
                        )}
                    </Space>
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
                            onShare={openGroupShare}
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
            <div ref={scrollContainerRef} className="h-full overflow-y-auto overflow-x-hidden">
                <Flex gap={12} className="mb-3" wrap>
                    {[
                        {
                            label: '设备总数',
                            value: stats.total,
                            className: 'text-blue-600',
                            tag: 'blue',
                            field: 'total',
                        },
                        {
                            label: '在线设备',
                            value: stats.online,
                            className: 'text-green-600',
                            tag: 'green',
                            field: 'online',
                        },
                        {
                            label: '离线设备',
                            value: stats.offline,
                            className: 'text-red-500',
                            tag: 'red',
                            field: 'offline',
                        },
                        {
                            label: '已启用',
                            value: stats.enabled,
                            className: 'text-purple-700',
                            tag: 'purple',
                            field: 'enabled',
                        },
                    ].map((item) => (
                        <Card
                            key={item.label}
                            size="small"
                            className="min-w-[140px] flex-1"
                            classNames={{ body: 'px-4 py-3' }}
                        >
                            <Flex justify="space-between" align="center" className="mb-2.5">
                                <span className="text-[13px] text-gray-500">{item.label}</span>
                                <span className={`text-lg font-semibold ${item.className}`}>
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
            <DeviceShareDrawer resource={sharing} onClose={() => setSharing(null)} />
            {historyDevice && (
                <DeviceHistoryDrawer
                    key={historyDevice.id}
                    device={historyDevice}
                    onClose={() => setHistoryDevice(null)}
                />
            )}
        </PageContainer>
    );
};

export default DevicePage;
