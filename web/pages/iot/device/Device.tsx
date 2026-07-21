import { DeleteOutlined, EditOutlined, PlusOutlined, ReloadOutlined } from '@ant-design/icons';
import { App, Button, Card, Empty, Flex, Input, Result, Skeleton, Space, Tag, Tooltip } from 'antd';
import { useMemo, useState } from 'react';
import { PageContainer } from '@/components/PageContainer';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { useLinkOptions } from '../link/link.service';
import type { Device } from './device.types';
import DeviceFormModal, { type DeviceFormValues } from './DeviceFormModal';
import DeviceGroupPanel from './DeviceGroupPanel';
import {
    useDeviceDelete,
    useDeviceGroupTreeWithCount,
    useDeviceList,
    useDeviceSave,
} from './device.service';

const EMPTY_DEVICES: Device.RealTimeData[] = [];

const DevicePage = () => {
    const { modal } = App.useApp();
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
    const [selectedGroupId, setSelectedGroupId] = useState<number | null>(null);
    const [formOpen, setFormOpen] = useState(false);
    const [editing, setEditing] = useState<Device.Item | null>(null);
    const { run: debouncedSearch } = useDebounceFn(setKeyword, 250);

    const { data, isLoading, refetch } = useDeviceList({ enabled: canQuery });
    const { data: groupTree = [] } = useDeviceGroupTreeWithCount({ enabled: canQuery });
    const { data: linkOptions = [] } = useLinkOptions({ enabled: canQuery });
    const saveMutation = useDeviceSave();
    const deleteMutation = useDeviceDelete();
    const devices = data?.list ?? EMPTY_DEVICES;

    const groupScope = useMemo(() => {
        if (!selectedGroupId) return null;
        const result = new Set<number>();
        const walk = (nodes: typeof groupTree): boolean => {
            for (const node of nodes) {
                if (node.id === selectedGroupId) {
                    const collect = (item: (typeof groupTree)[number]) => {
                        result.add(item.id);
                        item.children?.forEach(collect);
                    };
                    collect(node);
                    return true;
                }
                if (node.children && walk(node.children)) return true;
            }
            return false;
        };
        walk(groupTree);
        return result;
    }, [groupTree, selectedGroupId]);

    const filtered = useMemo(() => {
        const normalized = keyword.trim().toLowerCase();
        return devices.filter((device) => {
            const inGroup =
                selectedGroupId === null
                    ? true
                    : selectedGroupId === 0
                      ? !device.group_id
                      : !!device.group_id && !!groupScope?.has(device.group_id);
            if (!inGroup) return false;
            if (!normalized) return true;
            return [device.name, device.device_code, device.link_name, device.protocol_name].some(
                (value) => value?.toLowerCase().includes(normalized)
            );
        });
    }, [devices, groupScope, keyword, selectedGroupId]);

    const stats = useMemo(() => {
        const byProtocol = new Map<string, number>();
        let enabled = 0;
        for (const device of devices) {
            if (device.status === 'enabled') enabled++;
            const protocol = device.protocol_type || '未知';
            byProtocol.set(protocol, (byProtocol.get(protocol) ?? 0) + 1);
        }
        return { enabled, byProtocol: [...byProtocol.entries()] };
    }, [devices]);

    const ungroupedCount = useMemo(
        () => devices.filter((device) => !device.group_id).length,
        [devices]
    );

    const openCreate = () => {
        setEditing(null);
        setFormOpen(true);
    };
    const openEdit = (device: Device.Item) => {
        setEditing(device);
        setFormOpen(true);
    };
    const closeForm = () => {
        setFormOpen(false);
        setEditing(null);
    };
    const save = (values: DeviceFormValues) => {
        const {
            connection_mode: _mode,
            agent_id: _agentId,
            agent_endpoint_id: _endpointId,
            ...dto
        } = values;
        saveMutation.mutate(
            { ...dto, id: editing?.id, link_id: Number(dto.link_id) },
            { onSuccess: closeForm }
        );
    };
    const remove = (device: Device.Item) => {
        modal.confirm({
            title: `确认删除设备「${device.name}」？`,
            content: '删除后设备配置将停止参与后续南桥运行。',
            okText: '删除',
            okButtonProps: { danger: true },
            onOk: () => deleteMutation.mutateAsync(device.id),
        });
    };

    if (!canQuery) {
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询设备列表的权限" />
            </PageContainer>
        );
    }

    return (
        <PageContainer
            header={
                <Flex justify="space-between" align="center" gap={12} wrap>
                    <h3 className="m-0 text-base font-medium">设备管理</h3>
                    <Space wrap>
                        <DeviceGroupPanel
                            selectedGroupId={selectedGroupId}
                            onSelect={setSelectedGroupId}
                            canManageGroup={canManageGroup}
                            ungroupedCount={ungroupedCount}
                        />
                        <Input.Search
                            allowClear
                            className="w-60"
                            placeholder="设备名称 / 编码 / 类型"
                            onChange={(event) => debouncedSearch(event.target.value)}
                        />
                        <Tooltip title="刷新">
                            <Button
                                icon={<ReloadOutlined />}
                                loading={isLoading}
                                onClick={() => refetch()}
                            />
                        </Tooltip>
                        {canAdd && (
                            <Button type="primary" icon={<PlusOutlined />} onClick={openCreate}>
                                新建设备
                            </Button>
                        )}
                    </Space>
                </Flex>
            }
        >
            <Flex gap={12} className="mb-3" wrap>
                <Card size="small" className="min-w-40 flex-1">
                    <div className="text-sm text-slate-500">设备总数</div>
                    <div className="mt-1 text-2xl font-semibold text-blue-600">
                        {devices.length}
                    </div>
                    <Space size={[4, 4]} wrap>
                        {stats.byProtocol.map(([protocol, count]) => (
                            <Tag key={protocol} color="blue">
                                {protocol}: {count}
                            </Tag>
                        ))}
                    </Space>
                </Card>
                <Card size="small" className="min-w-40 flex-1">
                    <div className="text-sm text-slate-500">已启用</div>
                    <div className="mt-1 text-2xl font-semibold text-purple-600">
                        {stats.enabled} / {devices.length}
                    </div>
                </Card>
                <Card size="small" className="min-w-40 flex-1">
                    <div className="text-sm text-slate-500">运行状态</div>
                    <div className="mt-1 text-sm text-slate-500">南桥尚未接入，统一显示离线</div>
                </Card>
            </Flex>

            {isLoading && devices.length === 0 ? (
                <div className="grid grid-cols-1 gap-3 xl:grid-cols-2 2xl:grid-cols-4">
                    {['first', 'second', 'third', 'fourth'].map((key) => (
                        <Card key={key}>
                            <Skeleton active paragraph={{ rows: 4 }} />
                        </Card>
                    ))}
                </div>
            ) : filtered.length === 0 ? (
                <Empty className="py-12" description={keyword ? '搜索无结果' : '暂无设备数据'} />
            ) : (
                <div className="grid grid-cols-1 gap-3 xl:grid-cols-2 2xl:grid-cols-4">
                    {filtered.map((device) => (
                        <Card
                            key={device.id}
                            size="small"
                            title={
                                <div className="min-w-0">
                                    <div className="truncate font-semibold">{device.name}</div>
                                    <div className="truncate text-xs font-normal text-slate-400">
                                        {device.device_code || `设备 #${device.id}`}
                                    </div>
                                </div>
                            }
                            extra={
                                <Space size={2}>
                                    {canEdit && (
                                        <Button
                                            type="text"
                                            size="small"
                                            icon={<EditOutlined />}
                                            onClick={() => openEdit(device)}
                                        />
                                    )}
                                    {canDelete && (
                                        <Button
                                            type="text"
                                            danger
                                            size="small"
                                            icon={<DeleteOutlined />}
                                            onClick={() => remove(device)}
                                        />
                                    )}
                                </Space>
                            }
                        >
                            <div className="grid grid-cols-2 gap-2 text-sm">
                                <span className="text-slate-400">协议</span>
                                <span>{device.protocol_type}</span>
                                <span className="text-slate-400">设备类型</span>
                                <span className="truncate">{device.protocol_name}</span>
                                <span className="text-slate-400">链路</span>
                                <span className="truncate">{device.link_name}</span>
                                <span className="text-slate-400">设备时区</span>
                                <span>{device.timezone || '+08:00'}</span>
                                <span className="text-slate-400">状态</span>
                                <Tag color={device.status === 'enabled' ? 'success' : 'default'}>
                                    {device.status === 'enabled' ? '启用' : '禁用'}
                                </Tag>
                            </div>
                        </Card>
                    ))}
                </div>
            )}

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
