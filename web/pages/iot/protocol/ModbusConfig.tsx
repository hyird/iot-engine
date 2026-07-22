/**
 * Modbus 协议配置页面
 * 布局：左侧设备类型列表 + 右侧寄存器配置
 */

import { DownloadOutlined, UploadOutlined } from '@ant-design/icons';
import {
    Button,
    Card,
    Empty,
    Flex,
    Popconfirm,
    Result,
    Skeleton,
    Space,
    Tag,
    Tooltip,
    Tree,
} from 'antd';
import { type ReactNode, useCallback, useMemo, useRef, useState } from 'react';
import { PageContainer } from '@/components/PageContainer';
import { usePermissions } from '@/hooks/usePermission';
import { getGroupKey, reorderItemsByGroupOrder, reorderItemsWithinGroupOrder } from './grouping';
import { DeviceTypeModal } from './modbus/DeviceTypeModal';
import {
    ByteOrderOptions,
    buildRegisterGroupSections,
    DataTypeOptions,
    type DeviceTypeModalRef,
    formatScaleValue,
    getRegisterTypeMeta,
    normalizeModbusRegisters,
    normalizePacketConfig,
    REGISTER_CARD_GRID_STYLE,
    REGISTER_TYPE_META,
    REGISTER_TYPE_ORDER,
    type RegisterModalRef,
    RegisterTypeOptions,
} from './modbus/helpers';
import { RegisterModal } from './modbus/RegisterModal';
import {
    useProtocolConfigDelete,
    useProtocolConfigList,
    useProtocolConfigSave,
} from './protocol.service';
import type { Modbus } from './protocol.types';
import {
    SortableGroupItemList,
    SortableGroupSectionFrame,
    SortableGroupSectionList,
} from './SortableGroup';
import { useProtocolImportExport } from './useProtocolImportExport';

const ModbusConfigPage = () => {
    // 权限检查
    const { has } = usePermissions();
    const canQuery = has('iot:protocol:query');
    const canAdd = has('iot:protocol:add');
    const canEdit = has('iot:protocol:edit');
    const canDelete = has('iot:protocol:delete');
    const canImport = has('iot:protocol:import');
    const canExport = has('iot:protocol:export');

    // 设备类型列表查询
    const {
        data: configPage,
        isLoading: loadingTypes,
        refetch: refetchTypes,
    } = useProtocolConfigList({ protocol: 'Modbus' }, { enabled: canQuery });

    // 保存和删除 mutations
    const saveMutation = useProtocolConfigSave();
    const deleteMutation = useProtocolConfigDelete();

    // 导入导出
    const { exportConfigs, triggerImport, importing } = useProtocolImportExport('Modbus');

    // 当前选中的设备类型 ID（用户手动选择）
    const [selectedTypeId, setSelectedTypeId] = useState<string>();

    // Modal refs
    const deviceTypeModalRef = useRef<DeviceTypeModalRef>(null);
    const registerModalRef = useRef<RegisterModalRef>(null);

    // 设备类型列表（使用 useMemo 保持引用稳定）
    const types = useMemo(() => configPage?.list || [], [configPage?.list]);
    const emptyTypeDesc = types.length ? '未选择设备类型' : '暂无设备类型';

    // 计算当前激活的类型 ID：优先用户选择，否则默认第一个
    const activeTypeId = useMemo(() => {
        if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
            return selectedTypeId;
        }
        return types.length > 0 ? types[0].id : undefined;
    }, [selectedTypeId, types]);

    // 当前激活的设备类型
    const activeType = useMemo(() => {
        return types.find((t) => t.id === activeTypeId);
    }, [activeTypeId, types]);

    // 寄存器列表（派生状态，根据 activeTypeId 计算）
    const registers = useMemo<Modbus.Register[]>(() => {
        if (!activeType) return [];
        const config = activeType.config as Modbus.Config;
        return normalizeModbusRegisters(config?.registers);
    }, [activeType]);
    const registerGroups = useMemo(() => buildRegisterGroupSections(registers), [registers]);
    const writableRegisterCount = useMemo(
        () => registers.filter((register) => register.writable).length,
        [registers]
    );

    const saveRegisterConfig = useCallback(
        async (nextRegisters: Modbus.Register[]) => {
            if (!activeTypeId || !activeType) return;

            const config = activeType.config as Modbus.Config;
            if (nextRegisters.length === registers.length) {
                const isSameOrder =
                    nextRegisters.every(
                        (register, index) => register.id === registers[index]?.id
                    ) &&
                    nextRegisters.every(
                        (register, index) =>
                            getGroupKey(register.group) === getGroupKey(registers[index]?.group)
                    );
                if (isSameOrder) return;
            }

            await saveMutation.mutateAsync({
                id: activeTypeId,
                protocol: 'Modbus',
                config: {
                    byteOrder: config.byteOrder,
                    readInterval: config.readInterval,
                    packet: normalizePacketConfig(config.packet),
                    registers: nextRegisters,
                },
            });
            await refetchTypes();
        },
        [activeType, activeTypeId, refetchTypes, registers, saveMutation]
    );

    const handleRegisterGroupOrderChange = useCallback(
        async (nextOrder: string[]) => {
            await saveRegisterConfig(reorderItemsByGroupOrder(registers, nextOrder));
        },
        [registers, saveRegisterConfig]
    );

    const handleRegisterItemOrderChange = useCallback(
        async (groupKey: string, nextOrder: string[]) => {
            await saveRegisterConfig(reorderItemsWithinGroupOrder(registers, groupKey, nextOrder));
        },
        [registers, saveRegisterConfig]
    );

    // 加载状态（与数据加载状态同步）
    const loadingRegisters = loadingTypes;

    // ========== 设备类型操作 ==========

    const handleDeleteDeviceType = async () => {
        if (!activeTypeId) return;
        // 删除前记录下一个可选中的类型
        const idx = types.findIndex((t) => t.id === activeTypeId);
        const nextType = types[idx + 1] ?? types[idx - 1];
        await deleteMutation.mutateAsync(activeTypeId);
        setSelectedTypeId(nextType?.id);
    };

    // ========== 寄存器操作 ==========

    const handleDeleteRegister = async (registerId: string) => {
        if (!activeTypeId || !activeType) return;

        const config = activeType.config as Modbus.Config;
        const newConfig: Modbus.Config = {
            byteOrder: config.byteOrder,
            readInterval: config.readInterval,
            packet: normalizePacketConfig(config.packet),
            registers: registers.filter((register) => register.id !== registerId),
        };

        await saveMutation.mutateAsync({
            id: activeTypeId,
            protocol: 'Modbus',
            config: newConfig,
        });
    };

    const renderRegisterCard = (register: Modbus.Register, dragHandle?: ReactNode) => {
        const meta = getRegisterTypeMeta(register.registerType);
        const typeLabel = RegisterTypeOptions.find(
            (opt) => opt.value === register.registerType
        )?.label;
        const dataTypeLabel = DataTypeOptions.find((opt) => opt.value === register.dataType)?.label;
        const byteOrderLabel = ByteOrderOptions.find(
            (opt) => opt.value === register.byteOrder
        )?.label;
        const isWritableType =
            register.registerType === 'COIL' || register.registerType === 'HOLDING_REGISTER';
        const addressLabel = `${meta.prefix}${register.address}`;

        return (
            <Card
                key={register.id}
                size="small"
                hoverable
                className="h-full border-slate-200 shadow-[0_1px_4px_rgba(15,23,42,0.06)]"
                styles={{ body: { padding: 12 } }}
            >
                <Flex justify="space-between" gap={12} align="start" className="mb-2">
                    <div className="min-w-0 flex-1">
                        <div className="truncate text-sm font-semibold text-slate-800">
                            {register.name}
                        </div>
                        <div className="mt-0.5 text-[12px] text-slate-400">地址 {addressLabel}</div>
                    </div>
                    <Space size={4} className="shrink-0">
                        {dragHandle}
                        {canEdit && (
                            <Button
                                size="small"
                                type="link"
                                onClick={() => {
                                    if (activeTypeId)
                                        registerModalRef.current?.open(
                                            'edit',
                                            activeTypeId,
                                            register
                                        );
                                }}
                            >
                                编辑
                            </Button>
                        )}
                        {canDelete && (
                            <Popconfirm
                                title="确认删除？"
                                onConfirm={() => handleDeleteRegister(register.id)}
                            >
                                <Button size="small" danger type="link">
                                    删除
                                </Button>
                            </Popconfirm>
                        )}
                    </Space>
                </Flex>

                <Space size={6} wrap className="mb-2">
                    <Tag color={meta.color}>{typeLabel || meta.label}</Tag>
                    <Tag>{addressLabel}</Tag>
                    <Tag color="geekblue">{dataTypeLabel || register.dataType}</Tag>
                    {register.registerType === 'HOLDING_REGISTER' ||
                    register.registerType === 'INPUT_REGISTER' ? (
                        byteOrderLabel ? (
                            <Tag color="purple">字节序 {byteOrderLabel}</Tag>
                        ) : (
                            <Tag>字节序 继承设备</Tag>
                        )
                    ) : null}
                    {isWritableType ? (
                        register.writable ? (
                            <Tag color="orange">读写</Tag>
                        ) : (
                            <Tag>只读</Tag>
                        )
                    ) : (
                        <Tag>只读</Tag>
                    )}
                    {register.unit ? <Tag>{register.unit}</Tag> : null}
                    {typeof register.scale === 'number' && register.scale !== 1 ? (
                        <Tag color="geekblue">x{formatScaleValue(register.scale)}</Tag>
                    ) : null}
                    {typeof register.decimals === 'number' ? (
                        <Tag>小数 {register.decimals}</Tag>
                    ) : null}
                </Space>

                {register.remark ? (
                    <div className="text-xs leading-5 text-slate-500">{register.remark}</div>
                ) : (
                    <div className="text-xs leading-5 text-slate-400">暂无备注</div>
                )}
            </Card>
        );
    };

    // 权限检查
    if (!canQuery) {
        return (
            <PageContainer title="Modbus配置">
                <Result status="403" title="无权访问" subTitle="您没有权限访问此页面" />
            </PageContainer>
        );
    }

    return (
        <PageContainer title="Modbus配置">
            <div className="flex h-full min-h-0 overflow-hidden">
                {/* 左侧：设备类型列表 */}
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
                                        onClick={() => deviceTypeModalRef.current?.open('create')}
                                    >
                                        新增
                                    </Button>
                                )}
                                {canEdit && (
                                    <Button
                                        size="small"
                                        disabled={!activeTypeId}
                                        onClick={() =>
                                            deviceTypeModalRef.current?.open('edit', activeType)
                                        }
                                    >
                                        编辑
                                    </Button>
                                )}
                                {canDelete && (
                                    <Popconfirm
                                        title="确认删除该设备类型？"
                                        onConfirm={handleDeleteDeviceType}
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
                                            disabled={!types.length}
                                            onClick={() => exportConfigs(types)}
                                        />
                                    </Tooltip>
                                )}
                                {canImport && (
                                    <Tooltip title="导入">
                                        <Button
                                            size="small"
                                            icon={<UploadOutlined />}
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
                                    const config = t.config as Modbus.Config;
                                    const regCount = config?.registers?.length || 0;
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

                {/* 右侧：寄存器配置 */}
                <div className="h-full min-h-0 min-w-0 flex-1">
                    <Card
                        title={
                            activeType ? (
                                <Space wrap>
                                    <span>寄存器配置</span>
                                    <Tag>
                                        {ByteOrderOptions.find(
                                            (o) =>
                                                o.value ===
                                                (activeType.config as Modbus.Config)?.byteOrder
                                        )?.label || 'Big-endian'}
                                    </Tag>
                                    <Tag>
                                        间隔{' '}
                                        {(activeType.config as Modbus.Config)?.readInterval ?? 1}s
                                    </Tag>
                                    <Tag>
                                        组包 gap≤
                                        {
                                            normalizePacketConfig(
                                                (activeType.config as Modbus.Config)?.packet
                                            ).mergeGap
                                        }
                                    </Tag>
                                    <Tag>
                                        单包≤
                                        {
                                            normalizePacketConfig(
                                                (activeType.config as Modbus.Config)?.packet
                                            ).maxQuantity
                                        }
                                    </Tag>
                                    <Tag color="blue">{registers.length} 个寄存器</Tag>
                                    <Tag color="geekblue">{registerGroups.length} 个分组</Tag>
                                    {writableRegisterCount > 0 && (
                                        <Tag color="orange">{writableRegisterCount} 个可写</Tag>
                                    )}
                                </Space>
                            ) : types.length > 0 ? (
                                '请选择设备类型'
                            ) : (
                                '暂无设备类型'
                            )
                        }
                        className="flex h-full min-h-0 flex-col overflow-hidden"
                        styles={{
                            body: { flex: 1, minHeight: 0, overflow: 'auto', padding: 16 },
                        }}
                        extra={
                            activeTypeId &&
                            canAdd && (
                                <Button
                                    type="primary"
                                    onClick={() =>
                                        registerModalRef.current?.open('create', activeTypeId)
                                    }
                                >
                                    新增寄存器
                                </Button>
                            )
                        }
                    >
                        {!activeTypeId ? (
                            <Empty description={emptyTypeDesc} />
                        ) : loadingRegisters ? (
                            <Skeleton active paragraph={{ rows: 5 }} />
                        ) : registers.length === 0 ? (
                            <Empty description="暂无寄存器，点击右上角新增寄存器" />
                        ) : (
                            <SortableGroupSectionList
                                sections={registerGroups}
                                className="w-full"
                                disabled={saveMutation.isPending}
                                onOrderChange={handleRegisterGroupOrderChange}
                                empty={<Empty description="暂无寄存器，点击右上角新增寄存器" />}
                            >
                                {(group) => (
                                    <SortableGroupSectionFrame
                                        id={group.key}
                                        key={group.key}
                                        className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4"
                                        bodyClassName="mt-4"
                                        disabled={saveMutation.isPending}
                                        title={group.label}
                                        meta={
                                            <Space size={6} wrap>
                                                <Tag color="blue">{group.count} 个</Tag>
                                                {REGISTER_TYPE_ORDER.map((type) =>
                                                    group.typeCounts[type] > 0 ? (
                                                        <Tag
                                                            key={type}
                                                            color={REGISTER_TYPE_META[type].color}
                                                        >
                                                            {REGISTER_TYPE_META[type].short}{' '}
                                                            {group.typeCounts[type]}
                                                        </Tag>
                                                    ) : null
                                                )}
                                            </Space>
                                        }
                                    >
                                        <SortableGroupItemList
                                            items={group.registers}
                                            className="grid gap-3"
                                            style={REGISTER_CARD_GRID_STYLE}
                                            minItemWidth={240}
                                            disabled={saveMutation.isPending}
                                            onOrderChange={(nextOrder) =>
                                                handleRegisterItemOrderChange(group.key, nextOrder)
                                            }
                                        >
                                            {(register, dragHandle) =>
                                                renderRegisterCard(register, dragHandle)
                                            }
                                        </SortableGroupItemList>
                                    </SortableGroupSectionFrame>
                                )}
                            </SortableGroupSectionList>
                        )}
                    </Card>
                </div>
            </div>

            {/* 设备类型 Modal */}
            <DeviceTypeModal
                ref={deviceTypeModalRef}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />

            {/* 寄存器 Modal */}
            <RegisterModal
                ref={registerModalRef}
                types={types}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />
        </PageContainer>
    );
};

// ========== 设备类型 Modal ==========

export default ModbusConfigPage;
