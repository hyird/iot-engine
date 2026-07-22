/**
 * S7 协议配置页面
 * 布局：左侧设备类型列表 + 右侧寄存器配置
 */

import { DownloadOutlined, UploadOutlined } from '@ant-design/icons';
import {
    Button,
    Card,
    Col,
    Empty,
    Flex,
    Form,
    Input,
    InputNumber,
    Popconfirm,
    Result,
    Row,
    Select,
    Skeleton,
    Space,
    Switch,
    Tag,
    Tooltip,
    Tree,
} from 'antd';
import { type ReactNode, useCallback, useMemo, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { usePermissions } from '@/hooks/usePermission';
import {
    buildGroupSections,
    getGroupKey,
    normalizeGroupName,
    reorderItemsByGroupOrder,
    reorderItemsWithinGroupOrder,
} from './grouping';
import {
    useProtocolConfigDelete,
    useProtocolConfigList,
    useProtocolConfigSave,
} from './protocol.service';
import type { S7 } from './protocol.types';
import {
    SortableGroupItemList,
    SortableGroupSectionFrame,
    SortableGroupSectionList,
} from './SortableGroup';
import { AreaModal } from './s7/AreaModal';
import {
    AREA_CARD_GRID_STYLE,
    bitOnlyAreaTypes,
    buildConnectionConfig,
    buildRemoteTsapFromRackSlot,
    connectionTypeOptions,
    connectionTypeTips,
    type DeviceTypeFormValues,
    defaultConfig,
    formatTsapValue,
    getAreaAddressRangeText,
    getConnectionFormValues,
    getConnectionModeLabel,
    getConnectionModeOptions,
    getConnectionModeTip,
    getConnectionTypeLabel,
    getDataTypeSize,
    getPlcPreset,
    inferConnectionMode,
    normalizeAreaTypeForPlcModel,
    normalizeS7DataType,
    plcModelOptions,
    supportsS7Decimals,
    validateTsapValue,
} from './s7/helpers';
import { useProtocolImportExport } from './useProtocolImportExport';

const S7ConfigPage = () => {
    const { has } = usePermissions();
    const canQuery = has('iot:protocol:query');
    const canAdd = has('iot:protocol:add');
    const canEdit = has('iot:protocol:edit');
    const canDelete = has('iot:protocol:delete');
    const canImport = has('iot:protocol:import') && canAdd;
    const canExport = has('iot:protocol:export');

    const {
        data: configList,
        isLoading: loadingTypes,
        refetch,
    } = useProtocolConfigList({ protocol: 'S7' }, { enabled: canQuery });
    const saveMutation = useProtocolConfigSave();
    const deleteMutation = useProtocolConfigDelete();
    const { exportConfigs, triggerImport, exporting, importing } = useProtocolImportExport('S7');

    const [selectedTypeId, setSelectedTypeId] = useState<string>();
    const [deviceTypeModalOpen, setDeviceTypeModalOpen] = useState(false);
    const [editingDeviceType, setEditingDeviceType] = useState<boolean>(false);
    const [createForm] = Form.useForm<DeviceTypeFormValues>();
    const [areaModalOpen, setAreaModalOpen] = useState(false);
    const [editingAreaId, setEditingAreaId] = useState<string | null>(null);

    const types = useMemo(() => configList ?? [], [configList]);
    const emptyTypeDesc = types.length ? '未选择设备类型' : '暂无设备类型';
    const selectedPlcModel = Form.useWatch('plcModel', createForm) as S7.PlcModel | undefined;
    const selectedConnectionType = Form.useWatch('connectionType', createForm) as
        | S7.ConnectionType
        | undefined;
    const selectedConnectionMode = Form.useWatch('connectionMode', createForm) as
        | S7.ConnectionMode
        | undefined;
    const connectionTypeTip = connectionTypeTips[selectedConnectionType || 'PG'];
    const connectionModeTip = getConnectionModeTip(selectedPlcModel, selectedConnectionMode);
    const connectionModeSelectOptions = getConnectionModeOptions(selectedPlcModel);

    const activeTypeId = useMemo(() => {
        if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
            return selectedTypeId;
        }
        return types.length > 0 ? types[0].id : undefined;
    }, [selectedTypeId, types]);

    const activeType = useMemo(
        () => types.find((t) => t.id === activeTypeId),
        [activeTypeId, types]
    );

    const activeConfig = (activeType?.config as S7.Config) ?? null;
    const activeAreas = activeConfig?.areas ?? [];
    const areaGroups = useMemo(() => buildGroupSections(activeAreas), [activeAreas]);
    const areaGroupNames = useMemo(
        () =>
            Array.from(
                new Set(activeAreas.map((area) => normalizeGroupName(area.group)).filter(Boolean))
            ),
        [activeAreas]
    );
    const activeConnectionMode = activeConfig
        ? inferConnectionMode(activeConfig.plcModel, activeConfig.connection)
        : undefined;
    const activeConnectionPreset = activeConfig
        ? getConnectionFormValues(activeConfig.plcModel, activeConfig.connection)
        : null;

    const persistAreas = useCallback(
        async (nextAreas: S7.Area[]) => {
            if (!activeTypeId || !activeConfig) return;

            if (nextAreas.length === activeAreas.length) {
                const isSameOrder =
                    nextAreas.every((area, index) => area.id === activeAreas[index]?.id) &&
                    nextAreas.every(
                        (area, index) =>
                            getGroupKey(area.group) === getGroupKey(activeAreas[index]?.group)
                    );
                if (isSameOrder) return;
            }

            await saveMutation.mutateAsync({
                id: activeTypeId,
                protocol: 'S7',
                config: {
                    ...activeConfig,
                    areas: nextAreas,
                },
            });
            await refetch();
        },
        [activeAreas, activeConfig, activeTypeId, refetch, saveMutation]
    );

    const handleAreaGroupOrderChange = useCallback(
        async (nextOrder: string[]) => {
            await persistAreas(reorderItemsByGroupOrder(activeAreas, nextOrder));
        },
        [activeAreas, persistAreas]
    );

    const handleAreaItemOrderChange = useCallback(
        async (groupKey: string, nextOrder: string[]) => {
            await persistAreas(reorderItemsWithinGroupOrder(activeAreas, groupKey, nextOrder));
        },
        [activeAreas, persistAreas]
    );

    const renderAreaCard = (area: S7.Area, dragHandle?: ReactNode) => {
        const displayArea =
            normalizeAreaTypeForPlcModel(activeConfig?.plcModel, area.area) ?? area.area;
        const displayDataType = normalizeS7DataType(area.dataType);
        const addressRange = getAreaAddressRangeText({ ...area, area: displayArea });
        const canShowDecimals = supportsS7Decimals(displayDataType);
        const areaLocationLabel =
            displayArea === 'DB'
                ? `DB ${area.dbNumber ?? '-'}`
                : displayArea === 'V'
                  ? 'V 区'
                  : `${displayArea} 区`;

        return (
            <Card
                key={area.id}
                size="small"
                hoverable
                className="h-full border-slate-200 shadow-[0_1px_4px_rgba(15,23,42,0.06)]"
                styles={{ body: { padding: 12 } }}
            >
                <Flex justify="space-between" gap={12} align="start" className="mb-2">
                    <div className="min-w-0 flex-1">
                        <div className="truncate text-sm font-semibold text-slate-800">
                            {area.name}
                        </div>
                        <div className="mt-0.5 text-[12px] text-slate-400">
                            起始 {addressRange.start} · 结束 {addressRange.end}
                        </div>
                    </div>
                    <Space size={4} className="shrink-0">
                        {dragHandle}
                        {canEdit && (
                            <Button
                                size="small"
                                type="link"
                                onClick={() => {
                                    setEditingAreaId(area.id);
                                    setAreaModalOpen(true);
                                }}
                            >
                                编辑
                            </Button>
                        )}
                        {canDelete && (
                            <Popconfirm
                                title="确认删除该寄存器？"
                                onConfirm={async () => {
                                    if (!activeTypeId || !activeConfig) return;
                                    const nextAreas = activeConfig.areas.filter(
                                        (item) => item.id !== area.id
                                    );
                                    await saveMutation.mutateAsync({
                                        id: activeTypeId,
                                        protocol: 'S7',
                                        config: {
                                            ...activeConfig,
                                            areas: nextAreas,
                                        },
                                    });
                                    await refetch();
                                }}
                            >
                                <Button size="small" danger type="link">
                                    删除
                                </Button>
                            </Popconfirm>
                        )}
                    </Space>
                </Flex>

                <Space size={6} wrap className="mb-2">
                    <Tag color="blue">{displayArea}</Tag>
                    <Tag color="geekblue">{displayDataType}</Tag>
                    <Tag>{areaLocationLabel}</Tag>
                    <Tag>{area.size} 字节</Tag>
                    {area.writable ? <Tag color="orange">可写</Tag> : <Tag>只读</Tag>}
                    {area.unit ? <Tag>{area.unit}</Tag> : null}
                    {canShowDecimals && typeof area.decimals === 'number' ? (
                        <Tag>小数位数 {area.decimals}</Tag>
                    ) : null}
                    {displayDataType === 'BOOL' && typeof area.startBit === 'number' ? (
                        <Tag>位 {area.startBit}</Tag>
                    ) : null}
                </Space>

                {area.remark ? (
                    <div className="text-xs leading-5 text-slate-500">{area.remark}</div>
                ) : (
                    <div className="text-xs leading-5 text-slate-400">暂无备注</div>
                )}
            </Card>
        );
    };

    const handleDeleteType = async () => {
        if (!activeTypeId) return;
        const idx = types.findIndex((t) => t.id === activeTypeId);
        const nextType = types[idx + 1] ?? types[idx - 1];
        await deleteMutation.mutateAsync(activeTypeId);
        setSelectedTypeId(nextType?.id);
        await refetch();
    };

    const handleOpenCreateType = () => {
        setEditingDeviceType(false);
        setDeviceTypeModalOpen(true);
        createForm.setFieldsValue({
            deviceType: '',
            plcModel: 'S7-1200',
            ...getConnectionFormValues('S7-1200'),
            pollInterval: 5,
            storageInterval: 1,
            commandFastReadDuration: 60,
            commandFastReadInterval: 1,
            enabled: true,
            remark: '',
        });
    };

    const handleOpenEditType = () => {
        if (!activeType) return;
        const currentConfig = (activeType.config as S7.Config) ?? defaultConfig();
        setEditingDeviceType(true);
        setDeviceTypeModalOpen(true);
        createForm.setFieldsValue({
            deviceType: activeType.name,
            plcModel: currentConfig.plcModel ?? 'S7-1200',
            ...getConnectionFormValues(
                currentConfig.plcModel ?? 'S7-1200',
                currentConfig.connection
            ),
            pollInterval: currentConfig.pollInterval ?? 5,
            storageInterval: currentConfig.storageInterval ?? 1,
            commandFastReadDuration: currentConfig.commandFastReadDuration ?? 60,
            commandFastReadInterval: currentConfig.commandFastReadInterval ?? 1,
            enabled: activeType.enabled,
            remark: activeType.remark,
        });
    };

    const handlePlcModelChange = (plcModel: S7.PlcModel) => {
        const preset = getPlcPreset(plcModel);
        createForm.setFieldsValue({
            connectionMode: preset.mode,
            rack: preset.rack,
            slot: preset.slot,
            localTSAP: preset.localTSAP,
            remoteTSAP: preset.remoteTSAP,
        });
    };

    const handleConnectionModeChange = (connectionMode: S7.ConnectionMode) => {
        const plcModel =
            (createForm.getFieldValue('plcModel') as S7.PlcModel | undefined) ?? 'S7-1200';
        const preset = getPlcPreset(plcModel);
        if (connectionMode === 'RACK_SLOT') {
            createForm.setFieldsValue({
                rack: (createForm.getFieldValue('rack') as number | undefined) ?? preset.rack,
                slot: (createForm.getFieldValue('slot') as number | undefined) ?? preset.slot,
            });
            return;
        }

        const connectionType =
            (createForm.getFieldValue('connectionType') as S7.ConnectionType | undefined) ?? 'PG';
        const rack = (createForm.getFieldValue('rack') as number | undefined) ?? preset.rack;
        const slot = (createForm.getFieldValue('slot') as number | undefined) ?? preset.slot;
        const currentLocalTSAP = formatTsapValue(
            createForm.getFieldValue('localTSAP') as string | undefined
        );
        const currentRemoteTSAP = formatTsapValue(
            createForm.getFieldValue('remoteTSAP') as string | undefined
        );

        createForm.setFieldsValue({
            localTSAP: currentLocalTSAP ?? (preset.mode === 'TSAP' ? preset.localTSAP : '0100'),
            remoteTSAP:
                currentRemoteTSAP ??
                (preset.mode === 'TSAP'
                    ? preset.remoteTSAP
                    : buildRemoteTsapFromRackSlot(rack, slot, connectionType)),
        });
    };

    const handleSaveDeviceType = async () => {
        const values = await createForm.validateFields();
        if (editingDeviceType) {
            if (!activeTypeId || !activeType) return;
            const currentConfig = (activeType.config as S7.Config) ?? defaultConfig();
            const nextConfig: S7.Config = {
                ...currentConfig,
                plcModel: values.plcModel,
                connection: {
                    ...currentConfig.connection,
                    ...buildConnectionConfig(values),
                },
                pollInterval: values.pollInterval,
                storageInterval: values.storageInterval,
                commandFastReadDuration: values.commandFastReadDuration,
                commandFastReadInterval: values.commandFastReadInterval,
            };
            await saveMutation.mutateAsync({
                id: activeTypeId,
                protocol: 'S7',
                name: values.deviceType,
                enabled: values.enabled,
                config: nextConfig,
                remark: values.remark,
            });
        } else {
            const config = defaultConfig();
            const nextConfig: S7.Config = {
                ...config,
                deviceType: values.deviceType,
                plcModel: values.plcModel,
                pollInterval: values.pollInterval,
                storageInterval: values.storageInterval,
                commandFastReadDuration: values.commandFastReadDuration,
                commandFastReadInterval: values.commandFastReadInterval,
                connection: buildConnectionConfig(values),
            };
            await saveMutation.mutateAsync({
                protocol: 'S7',
                name: values.deviceType,
                enabled: values.enabled,
                config: nextConfig,
                remark: values.remark,
            });
        }
        setDeviceTypeModalOpen(false);
        setEditingDeviceType(false);
        createForm.resetFields();
        await refetch();
    };

    const loadingAreas = loadingTypes;
    const editingArea = activeConfig?.areas.find((area) => area.id === editingAreaId);

    if (!canQuery) {
        return (
            <PageContainer title="S7配置">
                <Result status="403" title="无权访问" subTitle="您没有权限访问此页面" />
            </PageContainer>
        );
    }

    return (
        <PageContainer title="S7配置">
            <div className="flex h-full min-h-0 overflow-hidden">
                <div className="h-full min-h-0 w-[360px] shrink-0 pr-3">
                    <Card
                        title="设备类型"
                        className="flex h-full min-h-0 flex-col overflow-hidden"
                        styles={{
                            body: { flex: 1, minHeight: 0, overflow: 'auto', padding: 16 },
                        }}
                        extra={
                            <Space size={4}>
                                {canAdd && (
                                    <Button
                                        size="small"
                                        type="primary"
                                        onClick={handleOpenCreateType}
                                    >
                                        新增
                                    </Button>
                                )}
                                {canEdit && (
                                    <Button
                                        size="small"
                                        disabled={!activeTypeId}
                                        onClick={handleOpenEditType}
                                    >
                                        编辑
                                    </Button>
                                )}
                                {canDelete && (
                                    <Popconfirm
                                        title="确认删除该设备类型？"
                                        onConfirm={handleDeleteType}
                                        disabled={!activeTypeId}
                                    >
                                        <Button size="small" danger disabled={!activeTypeId}>
                                            删除
                                        </Button>
                                    </Popconfirm>
                                )}
                                {canExport && (
                                    <Tooltip title="导出">
                                        <Button
                                            size="small"
                                            icon={<DownloadOutlined />}
                                            disabled={loadingTypes || importing}
                                            loading={exporting}
                                            onClick={exportConfigs}
                                        />
                                    </Tooltip>
                                )}
                                {canImport && (
                                    <Tooltip title="导入">
                                        <Button
                                            size="small"
                                            icon={<UploadOutlined />}
                                            disabled={exporting}
                                            loading={importing}
                                            onClick={triggerImport}
                                        />
                                    </Tooltip>
                                )}
                            </Space>
                        }
                    >
                        {loadingTypes ? (
                            <Skeleton active paragraph={{ rows: 6 }} />
                        ) : types.length === 0 ? (
                            <Empty description="暂无设备类型" />
                        ) : (
                            <Tree
                                blockNode
                                className="[&_.ant-tree-switcher]:hidden"
                                selectedKeys={activeTypeId ? [String(activeTypeId)] : []}
                                onSelect={(keys) => {
                                    if (keys.length > 0) {
                                        setSelectedTypeId(String(keys[0]));
                                    }
                                }}
                                treeData={types.map((t) => {
                                    const config = t.config as S7.Config;
                                    const regCount = config?.areas?.length || 0;
                                    return {
                                        key: String(t.id),
                                        title: (
                                            <Tooltip
                                                title={t.remark || '暂无备注'}
                                                placement="right"
                                            >
                                                <Flex
                                                    justify="space-between"
                                                    align="center"
                                                    className="h-8 p-1"
                                                >
                                                    <Space size={4}>
                                                        <span>{t.name}</span>
                                                        <Tag color="blue">{regCount}个寄存器</Tag>
                                                    </Space>
                                                    {t.enabled ? (
                                                        <Tag color="green">启用</Tag>
                                                    ) : (
                                                        <Tag color="red">禁用</Tag>
                                                    )}
                                                </Flex>
                                            </Tooltip>
                                        ),
                                    };
                                })}
                            />
                        )}
                    </Card>
                </div>

                <div className="h-full min-h-0 min-w-0 flex-1">
                    <Card
                        title={
                            activeType ? (
                                <Space>
                                    <span>寄存器配置</span>
                                    {activeType?.enabled ? (
                                        <Tag color="green">启用</Tag>
                                    ) : (
                                        <Tag color="red">禁用</Tag>
                                    )}
                                    <Tag color={activeConnectionMode === 'TSAP' ? 'cyan' : 'blue'}>
                                        {getConnectionModeLabel(activeConnectionMode)}
                                    </Tag>
                                    <Tag color="geekblue">{activeAreas.length} 个寄存器</Tag>
                                    <Tag color="purple">{areaGroups.length} 个分组</Tag>
                                    {activeConnectionMode === 'TSAP' ? (
                                        <Tag>
                                            TSAP {activeConnectionPreset?.localTSAP ?? '0100'} /{' '}
                                            {activeConnectionPreset?.remoteTSAP ?? '0100'}
                                        </Tag>
                                    ) : (
                                        <>
                                            <Tag color="blue">
                                                {getConnectionTypeLabel(
                                                    (activeType.config as S7.Config)?.connection
                                                        ?.connectionType
                                                )}
                                            </Tag>
                                            <Tag>
                                                Rack {activeConnectionPreset?.rack ?? 0} / Slot{' '}
                                                {activeConnectionPreset?.slot ?? 1}
                                            </Tag>
                                        </>
                                    )}
                                    <Tag>
                                        读取间隔{' '}
                                        {(activeType.config as S7.Config)?.pollInterval ?? 5}s
                                    </Tag>
                                </Space>
                            ) : types.length > 0 ? (
                                '请选择设备类型'
                            ) : (
                                '暂无设备类型'
                            )
                        }
                        className="flex h-full min-h-0 flex-col overflow-hidden"
                        styles={{
                            body: { flex: 1, minHeight: 0, overflow: 'auto', padding: 0 },
                        }}
                        extra={
                            canAdd &&
                            activeTypeId && (
                                <Button
                                    size="small"
                                    type="primary"
                                    onClick={() => {
                                        setEditingAreaId(null);
                                        setAreaModalOpen(true);
                                    }}
                                >
                                    新增寄存器
                                </Button>
                            )
                        }
                    >
                        {!activeType ? (
                            <Empty description={emptyTypeDesc} />
                        ) : loadingAreas ? (
                            <Skeleton active paragraph={{ rows: 6 }} />
                        ) : activeAreas.length === 0 ? (
                            <Empty description="暂无寄存器，点击右上角新增寄存器" />
                        ) : (
                            <SortableGroupSectionList
                                sections={areaGroups}
                                className="w-full p-4"
                                disabled={saveMutation.isPending || !canEdit}
                                onOrderChange={handleAreaGroupOrderChange}
                                empty={<Empty description="暂无寄存器，点击右上角新增寄存器" />}
                            >
                                {(group) => {
                                    const writableCount = group.items.filter(
                                        (area) => area.writable
                                    ).length;

                                    return (
                                        <SortableGroupSectionFrame
                                            id={group.key}
                                            key={group.key}
                                            className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4"
                                            bodyClassName="mt-4"
                                            disabled={saveMutation.isPending || !canEdit}
                                            title={group.label}
                                            meta={
                                                <Space size={6} wrap>
                                                    <Tag color="blue">{group.count} 个</Tag>
                                                    {writableCount > 0 && (
                                                        <Tag color="orange">
                                                            {writableCount} 个可写
                                                        </Tag>
                                                    )}
                                                </Space>
                                            }
                                        >
                                            <SortableGroupItemList
                                                items={group.items}
                                                className="grid gap-3"
                                                style={AREA_CARD_GRID_STYLE}
                                                minItemWidth={320}
                                                disabled={saveMutation.isPending || !canEdit}
                                                empty={<Empty description="暂无寄存器" />}
                                                onOrderChange={(nextOrder) =>
                                                    handleAreaItemOrderChange(group.key, nextOrder)
                                                }
                                            >
                                                {(area, dragHandle) =>
                                                    renderAreaCard(area, dragHandle)
                                                }
                                            </SortableGroupItemList>
                                        </SortableGroupSectionFrame>
                                    );
                                }}
                            </SortableGroupSectionList>
                        )}
                    </Card>
                </div>
            </div>

            <FormModal
                title={editingDeviceType ? '编辑设备类型' : '新建设备类型'}
                open={deviceTypeModalOpen}
                confirmLoading={saveMutation.isPending}
                onCancel={() => {
                    setDeviceTypeModalOpen(false);
                    setEditingDeviceType(false);
                    createForm.resetFields();
                }}
                onOk={handleSaveDeviceType}
                destroyOnHidden
            >
                <Form
                    form={createForm}
                    layout="vertical"
                    initialValues={{
                        deviceType: '',
                        plcModel: 'S7-1200',
                        connectionMode: 'RACK_SLOT',
                        connectionType: 'PG',
                        rack: 0,
                        slot: 1,
                        localTSAP: '0100',
                        remoteTSAP: '0101',
                        probeMode: 'STANDARD',
                        handshakeTimeout: 5000,
                        directProbeTimeout: 5000,
                        pollInterval: 5,
                        storageInterval: 1,
                        commandFastReadDuration: 60,
                        commandFastReadInterval: 1,
                        enabled: true,
                        remark: '',
                    }}
                >
                    <Form.Item
                        name="deviceType"
                        label="名称"
                        rules={[{ required: true, message: '请输入名称' }]}
                        extra="用于区分同一协议下不同设备类别"
                    >
                        <Input placeholder="例如: 温湿度传感器" maxLength={64} />
                    </Form.Item>
                    <Form.Item
                        name="plcModel"
                        label="PLC型号"
                        rules={[{ required: true, message: '请选择PLC型号' }]}
                        extra="切换型号会自动带入对应的默认连接模式和参数"
                    >
                        <Select
                            options={plcModelOptions.map(({ value, label }) => ({ value, label }))}
                            onChange={(value) => handlePlcModelChange(value as S7.PlcModel)}
                        />
                    </Form.Item>
                    <Form.Item
                        name="connectionMode"
                        label="连接模式"
                        rules={[{ required: true, message: '请选择连接模式' }]}
                        extra={connectionModeTip}
                    >
                        <Select
                            options={connectionModeSelectOptions}
                            onChange={(value) =>
                                handleConnectionModeChange(value as S7.ConnectionMode)
                            }
                        />
                    </Form.Item>
                    {selectedConnectionMode === 'TSAP' ? (
                        <Row gutter={12}>
                            <Col xs={24} sm={12}>
                                <Form.Item
                                    name="localTSAP"
                                    label="本地 TSAP"
                                    rules={[
                                        { required: true, message: '请输入本地 TSAP' },
                                        { validator: validateTsapValue },
                                    ]}
                                    extra="支持 4D57、0200、0x4D57、4D.57"
                                >
                                    <Input placeholder="例如 4D57" />
                                </Form.Item>
                            </Col>
                            <Col xs={24} sm={12}>
                                <Form.Item
                                    name="remoteTSAP"
                                    label="远端 TSAP"
                                    rules={[
                                        { required: true, message: '请输入远端 TSAP' },
                                        { validator: validateTsapValue },
                                    ]}
                                    extra="S7-200 常见值为 4D57 或 0200"
                                >
                                    <Input placeholder="例如 4D57" />
                                </Form.Item>
                            </Col>
                        </Row>
                    ) : (
                        <>
                            <Form.Item
                                name="connectionType"
                                label="连接类型"
                                rules={[{ required: true, message: '请选择连接类型' }]}
                                extra={connectionTypeTip}
                            >
                                <Select options={connectionTypeOptions} />
                            </Form.Item>
                            <Row gutter={12}>
                                <Col xs={24} sm={12}>
                                    <Form.Item
                                        name="rack"
                                        label="Rack"
                                        rules={[{ required: true, message: '请输入 Rack' }]}
                                        extra="西门子机架号，通常从 0 开始"
                                    >
                                        <InputNumber min={0} className="w-full" />
                                    </Form.Item>
                                </Col>
                                <Col xs={24} sm={12}>
                                    <Form.Item
                                        name="slot"
                                        label="Slot"
                                        rules={[{ required: true, message: '请输入 Slot' }]}
                                        extra="CPU 或通信模块所在槽位"
                                    >
                                        <InputNumber min={0} className="w-full" />
                                    </Form.Item>
                                </Col>
                            </Row>
                        </>
                    )}
                    <Form.Item
                        name="probeMode"
                        label="连接探测模式"
                        rules={[{ required: true, message: '请选择探测模式' }]}
                        extra="标准模式完成 ISO-CC 和通信协商后才读数据；兼容模式直接读；自动模式仅在标准握手超时后降级"
                    >
                        <Select
                            options={[
                                { value: 'STANDARD', label: '标准模式（推荐）' },
                                { value: 'COMPATIBLE', label: '兼容模式（直接读）' },
                                { value: 'AUTO', label: '自动降级' },
                            ]}
                        />
                    </Form.Item>
                    <Row gutter={12}>
                        <Col xs={24} sm={12}>
                            <Form.Item
                                name="handshakeTimeout"
                                label="握手超时"
                                rules={[{ required: true, message: '请输入握手超时' }]}
                            >
                                <InputNumber
                                    min={1000}
                                    max={30000}
                                    step={1000}
                                    className="w-full"
                                    addonAfter="ms"
                                />
                            </Form.Item>
                        </Col>
                        <Col xs={24} sm={12}>
                            <Form.Item
                                name="directProbeTimeout"
                                label="兼容探测超时"
                                rules={[{ required: true, message: '请输入兼容探测超时' }]}
                            >
                                <InputNumber
                                    min={1000}
                                    max={30000}
                                    step={1000}
                                    className="w-full"
                                    addonAfter="ms"
                                />
                            </Form.Item>
                        </Col>
                    </Row>
                    <Form.Item
                        name="pollInterval"
                        label="轮询间隔（秒）"
                        rules={[{ required: true, message: '请输入轮询间隔' }]}
                        extra="数值越小采集越频繁，建议 1~300 秒之间按实际场景调整"
                    >
                        <InputNumber min={1} max={3600} className="w-full" addonAfter="秒" />
                    </Form.Item>
                    <Form.Item
                        name="storageInterval"
                        label="存储间隔（秒）"
                        rules={[{ required: true, message: '请输入存储间隔' }]}
                        extra="历史数据入库的最小间隔，1 表示每次读取都存储"
                    >
                        <InputNumber min={1} max={86400} className="w-full" addonAfter="秒" />
                    </Form.Item>
                    <Row gutter={12}>
                        <Col xs={24} sm={12}>
                            <Form.Item
                                name="commandFastReadDuration"
                                label="下发快读窗口"
                                rules={[{ required: true, message: '请输入快读窗口' }]}
                                extra="下发成功后保持快读的时长，0 表示关闭"
                            >
                                <InputNumber
                                    min={0}
                                    max={3600}
                                    className="w-full"
                                    addonAfter="秒"
                                />
                            </Form.Item>
                        </Col>
                        <Col xs={24} sm={12}>
                            <Form.Item
                                name="commandFastReadInterval"
                                label="快读间隔"
                                rules={[{ required: true, message: '请输入快读间隔' }]}
                                extra="快读窗口内的读取间隔"
                            >
                                <InputNumber min={1} max={60} className="w-full" addonAfter="秒" />
                            </Form.Item>
                        </Col>
                    </Row>
                    <Form.Item name="remark" label="备注">
                        <Input.TextArea rows={3} placeholder="备注说明" />
                    </Form.Item>
                    <Form.Item name="enabled" label="启用" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                </Form>
            </FormModal>

            <AreaModal
                open={areaModalOpen}
                mode={editingArea ? 'edit' : 'create'}
                initialValue={
                    editingArea
                        ? {
                              ...editingArea,
                              area:
                                  normalizeAreaTypeForPlcModel(
                                      activeConfig?.plcModel,
                                      editingArea.area
                                  ) ?? editingArea.area,
                              dataType: bitOnlyAreaTypes.includes(editingArea.area as S7.AreaType)
                                  ? 'BOOL'
                                  : editingArea.area === 'CT' || editingArea.area === 'TM'
                                    ? 'UINT16'
                                    : normalizeS7DataType(editingArea.dataType),
                              size: bitOnlyAreaTypes.includes(editingArea.area as S7.AreaType)
                                  ? 1
                                  : editingArea.area === 'CT' || editingArea.area === 'TM'
                                    ? getDataTypeSize('UINT16')
                                    : editingArea.size,
                              dbNumber:
                                  normalizeAreaTypeForPlcModel(
                                      activeConfig?.plcModel,
                                      editingArea.area
                                  ) === 'V'
                                      ? undefined
                                      : editingArea.dbNumber,
                              startBit: editingArea.startBit,
                          }
                        : undefined
                }
                plcModel={activeConfig?.plcModel}
                groupOptions={areaGroupNames}
                onCancel={() => setAreaModalOpen(false)}
                onSubmit={async (value) => {
                    if (!activeTypeId || !activeConfig) return;
                    const nextAreas = activeConfig.areas.some((area) => area.id === value.id)
                        ? activeConfig.areas.map((area) => (area.id === value.id ? value : area))
                        : [...activeConfig.areas, value];
                    await saveMutation.mutateAsync({
                        id: activeTypeId,
                        protocol: 'S7',
                        config: {
                            ...activeConfig,
                            areas: nextAreas,
                        },
                    });
                    await refetch();
                    setAreaModalOpen(false);
                    setEditingAreaId(null);
                }}
            />
        </PageContainer>
    );
};

export default S7ConfigPage;
