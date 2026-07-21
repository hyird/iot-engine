/**
 * Modbus 协议配置页面
 * 布局：左侧设备类型列表 + 右侧寄存器配置
 */

import { DownloadOutlined, UploadOutlined } from '@ant-design/icons';
import {
    AutoComplete,
    Button,
    Card,
    Empty,
    Flex,
    Form,
    Input,
    InputNumber,
    Modal,
    Popconfirm,
    Result,
    Select,
    Skeleton,
    Space,
    Switch,
    Tag,
    Tooltip,
    Tree,
} from 'antd';
import {
    forwardRef,
    type ReactNode,
    useCallback,
    useImperativeHandle,
    useMemo,
    useRef,
    useState,
} from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { usePermissions } from '@/hooks/usePermission';
import {
    buildGroupSections,
    getGroupKey,
    reorderItemsByGroupOrder,
    reorderItemsWithinGroupOrder,
} from './grouping';
import {
    useProtocolConfigDelete,
    useProtocolConfigList,
    useProtocolConfigSave,
} from './protocol.service';
import type { Modbus, ModbusDictConfig, Protocol } from './protocol.types';
import {
    SortableGroupItemList,
    SortableGroupSectionFrame,
    SortableGroupSectionList,
} from './SortableGroup';
import { useFilterableGroupOptions } from './useFilterableGroupOptions';
import { useProtocolImportExport } from './useProtocolImportExport';

/** 寄存器类型选项 */
const RegisterTypeOptions: { value: Modbus.RegisterType; label: string }[] = [
    { value: 'COIL', label: '0X - 线圈 (Coil)' },
    { value: 'DISCRETE_INPUT', label: '1X - 离散输入 (Discrete Input)' },
    { value: 'INPUT_REGISTER', label: '3X - 输入寄存器 (Input Register)' },
    { value: 'HOLDING_REGISTER', label: '4X - 保持寄存器 (Holding Register)' },
];

/** 数据类型选项 */
const DataTypeOptions: { value: Modbus.DataType; label: string; quantity: number }[] = [
    { value: 'BOOL', label: 'BOOL (1 bit)', quantity: 1 },
    { value: 'INT16', label: 'INT16 (16位有符号)', quantity: 1 },
    { value: 'UINT16', label: 'UINT16 (16位无符号)', quantity: 1 },
    { value: 'INT32', label: 'INT32 (32位有符号)', quantity: 2 },
    { value: 'UINT32', label: 'UINT32 (32位无符号)', quantity: 2 },
    { value: 'FLOAT32', label: 'FLOAT32 (32位浮点)', quantity: 2 },
    { value: 'INT64', label: 'INT64 (64位有符号)', quantity: 4 },
    { value: 'UINT64', label: 'UINT64 (64位无符号)', quantity: 4 },
    { value: 'DOUBLE', label: 'DOUBLE (64位浮点)', quantity: 4 },
];

/** 字节序选项 */
const ByteOrderOptions: { value: Modbus.ByteOrder; label: string }[] = [
    { value: 'BIG_ENDIAN', label: 'Big-endian' },
    { value: 'LITTLE_ENDIAN', label: 'Little-endian' },
    { value: 'BIG_ENDIAN_BYTE_SWAP', label: 'Big-endian byte swap' },
    { value: 'LITTLE_ENDIAN_BYTE_SWAP', label: 'Little-endian byte swap' },
];

const DEFAULT_PACKET_MERGE_GAP = 100;
const DEFAULT_PACKET_MAX_QUANTITY = 125;

const pairedFormItemClassName = 'min-w-0 flex-1';
const numericInputClassName = 'min-w-0 flex-1';
const numericUnitClassName = 'pointer-events-none !w-20 text-center';

const formatScaleValue = (value: number | string | undefined | null) => {
    if (value === null || value === undefined || value === '') return '';
    const numericValue = Number(value);
    if (!Number.isFinite(numericValue)) return String(value);
    return numericValue
        .toFixed(6)
        .replace(/(\.\d*?)0+$/, '$1')
        .replace(/\.$/, '');
};

const normalizePacketConfig = (packet?: Modbus.PacketConfig): Required<Modbus.PacketConfig> => {
    const mergeGapRaw = Number(packet?.mergeGap);
    const maxQuantityRaw = Number(packet?.maxQuantity);

    const mergeGap = Number.isFinite(mergeGapRaw)
        ? Math.min(2000, Math.max(0, Math.floor(mergeGapRaw)))
        : DEFAULT_PACKET_MERGE_GAP;
    const maxQuantity = Number.isFinite(maxQuantityRaw)
        ? Math.min(125, Math.max(1, Math.floor(maxQuantityRaw)))
        : DEFAULT_PACKET_MAX_QUANTITY;

    return { mergeGap, maxQuantity };
};

const REGISTER_TYPE_ORDER: Modbus.RegisterType[] = [
    'COIL',
    'DISCRETE_INPUT',
    'INPUT_REGISTER',
    'HOLDING_REGISTER',
];

const REGISTER_TYPE_META: Record<
    Modbus.RegisterType,
    { label: string; color: string; prefix: string; short: string }
> = {
    COIL: { label: '0X - 线圈 (Coil)', color: 'green', prefix: '0X', short: '0X' },
    DISCRETE_INPUT: {
        label: '1X - 离散输入 (Discrete Input)',
        color: 'blue',
        prefix: '1X',
        short: '1X',
    },
    INPUT_REGISTER: {
        label: '3X - 输入寄存器 (Input Register)',
        color: 'cyan',
        prefix: '3X',
        short: '3X',
    },
    HOLDING_REGISTER: {
        label: '4X - 保持寄存器 (Holding Register)',
        color: 'orange',
        prefix: '4X',
        short: '4X',
    },
};

const REGISTER_CARD_GRID_STYLE = {
    gridTemplateColumns: 'repeat(auto-fill, minmax(240px, 1fr))',
};

const normalizeGroupName = (group?: string) => group?.trim() || '';

interface RegisterGroupSection {
    key: string;
    label: string;
    count: number;
    registers: Modbus.Register[];
    typeCounts: Record<Modbus.RegisterType, number>;
}

const buildRegisterGroupSections = (registers: Modbus.Register[]): RegisterGroupSection[] => {
    return buildGroupSections(registers).map((section) => {
        const typeCounts: Record<Modbus.RegisterType, number> = {
            COIL: 0,
            DISCRETE_INPUT: 0,
            HOLDING_REGISTER: 0,
            INPUT_REGISTER: 0,
        };

        for (const register of section.items) {
            typeCounts[register.registerType]++;
        }

        return {
            key: section.key,
            label: section.label,
            count: section.count,
            registers: section.items,
            typeCounts,
        };
    });
};

/** 生成唯一 ID（兼容非安全上下文） */
const generateId = (): string =>
    '10000000-1000-4000-8000-100000000000'.replace(/[018]/g, (c) =>
        (+c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
    );

/** 根据数据类型获取寄存器数量 */
const getQuantityByDataType = (dataType: Modbus.DataType): number => {
    const opt = DataTypeOptions.find((o) => o.value === dataType);
    return opt?.quantity ?? 1;
};

/** 检查寄存器地址是否冲突 */
const checkAddressConflict = (
    registers: Modbus.Register[],
    newRegister: { registerType: Modbus.RegisterType; address: number; quantity: number },
    excludeId?: string
): { conflict: boolean; conflictWith?: Modbus.Register } => {
    const newStart = newRegister.address;
    const newEnd = newRegister.address + newRegister.quantity - 1;

    for (const reg of registers) {
        // 跳过自身（编辑模式）
        if (excludeId && reg.id === excludeId) continue;
        // 只检查同类型寄存器
        if (reg.registerType !== newRegister.registerType) continue;

        const existStart = reg.address;
        const existEnd = reg.address + reg.quantity - 1;

        // 检查地址范围是否重叠
        if (!(newEnd < existStart || newStart > existEnd)) {
            return { conflict: true, conflictWith: reg };
        }
    }

    return { conflict: false };
};

/** 设备类型 Modal Ref */
interface DeviceTypeModalRef {
    open: (mode: 'create' | 'edit', data?: Protocol.Item) => void;
}

/** 寄存器 Modal Ref */
interface RegisterModalRef {
    open: (mode: 'create' | 'edit', typeId: string, register?: Modbus.Register) => void;
}

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
        return config?.registers || [];
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
            registers: config.registers.filter((r) => r.id !== registerId),
        };

        await saveMutation.mutateAsync({
            id: activeTypeId,
            protocol: 'Modbus',
            config: newConfig,
        });
    };

    const renderRegisterCard = (register: Modbus.Register, dragHandle?: ReactNode) => {
        const meta = REGISTER_TYPE_META[register.registerType];
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

interface DeviceTypeModalProps {
    onSuccess?: () => void;
    saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const DeviceTypeModal = forwardRef<DeviceTypeModalRef, DeviceTypeModalProps>(
    ({ onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [mode, setMode] = useState<'create' | 'edit'>('create');
        const [current, setCurrent] = useState<Protocol.Item>();
        const [form] = Form.useForm();

        useImperativeHandle(ref, () => ({
            open(m, data) {
                setMode(m);
                setCurrent(data);
                form.resetFields();
                if (data) {
                    const config = data.config as Modbus.Config;
                    const packet = normalizePacketConfig(config?.packet);
                    form.setFieldsValue({
                        name: data.name,
                        enabled: data.enabled,
                        byteOrder: config?.byteOrder || 'BIG_ENDIAN',
                        readInterval: Number(config?.readInterval) || 1,
                        storageInterval: Number(config?.storageInterval) || 1,
                        commandFastReadDuration: Number(config?.commandFastReadDuration ?? 60),
                        commandFastReadInterval: Number(config?.commandFastReadInterval ?? 1),
                        packetMergeGap: packet.mergeGap,
                        packetMaxQuantity: packet.maxQuantity,
                        remark: data.remark,
                    });
                } else {
                    form.setFieldsValue({
                        enabled: true,
                        byteOrder: 'BIG_ENDIAN',
                        readInterval: 1,
                        storageInterval: 1,
                        commandFastReadDuration: 60,
                        commandFastReadInterval: 1,
                        packetMergeGap: DEFAULT_PACKET_MERGE_GAP,
                        packetMaxQuantity: DEFAULT_PACKET_MAX_QUANTITY,
                    });
                }
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            const values = await form.validateFields();
            const existingConfig = (current?.config as Modbus.Config) || { registers: [] };
            const packet = normalizePacketConfig({
                mergeGap: values.packetMergeGap,
                maxQuantity: values.packetMaxQuantity,
            });

            await saveMutation.mutateAsync({
                id: current?.id,
                protocol: 'Modbus',
                name: values.name,
                enabled: values.enabled,
                config: {
                    byteOrder: values.byteOrder,
                    readInterval: values.readInterval,
                    storageInterval: values.storageInterval,
                    commandFastReadDuration: values.commandFastReadDuration,
                    commandFastReadInterval: values.commandFastReadInterval,
                    packet,
                    registers: existingConfig.registers || [],
                },
                remark: values.remark,
            });

            onSuccess?.();
            setOpen(false);
        };

        return (
            <FormModal
                title={mode === 'create' ? '新增设备类型' : '编辑设备类型'}
                open={open}
                onOk={handleOk}
                onCancel={() => setOpen(false)}
                confirmLoading={saveMutation.isPending}
                forceRender
            >
                <Form form={form} layout="vertical">
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="名称"
                            name="name"
                            rules={[{ required: true, message: '请输入名称' }]}
                            className={pairedFormItemClassName}
                        >
                            <Input placeholder="如：温湿度传感器、电表" maxLength={64} />
                        </Form.Item>
                        <Form.Item
                            label="字节序"
                            name="byteOrder"
                            rules={[{ required: true, message: '请选择字节序' }]}
                            extra="不同字节序将影响寄存器值解析"
                            className={pairedFormItemClassName}
                        >
                            <Select options={ByteOrderOptions} />
                        </Form.Item>
                    </Flex>
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="读取间隔（秒）"
                            name="readInterval"
                            extra="数值越小采集越频繁，建议按设备负载设置间隔"
                            className={pairedFormItemClassName}
                        >
                            <Space.Compact block>
                                <InputNumber min={1} max={3600} className={numericInputClassName} />
                                <Input
                                    value="秒"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：秒"
                                />
                            </Space.Compact>
                        </Form.Item>
                        <Form.Item
                            label="存储间隔（秒）"
                            name="storageInterval"
                            extra="历史数据入库的最小间隔，1 表示每次读取都存储"
                            className={pairedFormItemClassName}
                        >
                            <Space.Compact block>
                                <InputNumber
                                    min={1}
                                    max={86400}
                                    className={numericInputClassName}
                                />
                                <Input
                                    value="秒"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：秒"
                                />
                            </Space.Compact>
                        </Form.Item>
                    </Flex>
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="下发快读窗口"
                            name="commandFastReadDuration"
                            className={pairedFormItemClassName}
                            extra="下发成功后保持快读的时长，0 表示关闭"
                        >
                            <Space.Compact block>
                                <InputNumber min={0} max={3600} className={numericInputClassName} />
                                <Input
                                    value="秒"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：秒"
                                />
                            </Space.Compact>
                        </Form.Item>
                        <Form.Item
                            label="快读间隔"
                            name="commandFastReadInterval"
                            className={pairedFormItemClassName}
                            extra="快读窗口内的读取间隔"
                        >
                            <Space.Compact block>
                                <InputNumber min={1} max={60} className={numericInputClassName} />
                                <Input
                                    value="秒"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：秒"
                                />
                            </Space.Compact>
                        </Form.Item>
                    </Flex>
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="组包地址间隙"
                            name="packetMergeGap"
                            className={pairedFormItemClassName}
                            extra="地址间隙 <= 该值时会合并成同一读包，0 表示只合并连续地址"
                        >
                            <Space.Compact block>
                                <InputNumber min={0} max={2000} className={numericInputClassName} />
                                <Input
                                    value="寄存器"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：寄存器"
                                />
                            </Space.Compact>
                        </Form.Item>
                        <Form.Item
                            label="单包最大寄存器数"
                            name="packetMaxQuantity"
                            className={pairedFormItemClassName}
                            extra="每个读包最多读取的字寄存器数量"
                        >
                            <Space.Compact block>
                                <InputNumber min={1} max={125} className={numericInputClassName} />
                                <Input
                                    value="个"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：个"
                                />
                            </Space.Compact>
                        </Form.Item>
                    </Flex>
                    <Form.Item label="备注" name="remark">
                        <Input.TextArea rows={3} placeholder="备注说明" />
                    </Form.Item>
                    <Form.Item label="启用" name="enabled" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);

// ========== 寄存器 Modal ==========

interface RegisterModalProps {
    types: Protocol.Item[];
    onSuccess?: () => void;
    saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const RegisterModal = forwardRef<RegisterModalRef, RegisterModalProps>(
    ({ types, onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [mode, setMode] = useState<'create' | 'edit'>('create');
        const [typeId, setTypeId] = useState<string>();
        const [current, setCurrent] = useState<Modbus.Register>();
        const [form] = Form.useForm();

        // 监听寄存器类型和数据类型变化
        const registerType = Form.useWatch('registerType', form);
        const dataType = Form.useWatch('dataType', form);
        const groupNames = useMemo(() => {
            const currentType = types.find((t) => t.id === typeId);
            const config = currentType?.config as Modbus.Config | undefined;
            const groups = new Set<string>();

            for (const register of config?.registers || []) {
                const group = normalizeGroupName(register.group);
                if (group) groups.add(group);
            }

            const currentGroup = normalizeGroupName(current?.group);
            if (currentGroup) groups.add(currentGroup);

            return Array.from(groups);
        }, [current?.group, typeId, types]);
        const groupOptions = useFilterableGroupOptions(groupNames);

        useImperativeHandle(ref, () => ({
            open(m, t, register) {
                setMode(m);
                setTypeId(t);
                setCurrent(register);
                form.resetFields();
                if (register) {
                    form.setFieldsValue({
                        ...register,
                        group: normalizeGroupName(register.group) || undefined,
                        scale: typeof register.scale === 'number' ? register.scale : 1,
                        boolLabel0: register.dictConfig?.items?.find((i) => i.key === '0')?.label,
                        boolLabel1: register.dictConfig?.items?.find((i) => i.key === '1')?.label,
                    });
                } else {
                    form.setFieldsValue({
                        registerType: 'HOLDING_REGISTER',
                        dataType: 'INT16',
                        writable: false,
                        scale: 1,
                    });
                }
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            if (!typeId) return;
            const values = await form.validateFields();

            const type = types.find((t) => t.id === typeId);
            if (!type) return;

            const config = type.config as Modbus.Config;
            const actualQuantity = getQuantityByDataType(values.dataType);

            // 检查地址 + 数量是否溢出 uint16 范围
            if (values.address + actualQuantity - 1 > 65535) {
                Modal.error({
                    title: '地址溢出',
                    content: `地址 ${values.address} + 数量 ${actualQuantity} 超出范围（末地址不能超过 65535）`,
                });
                return;
            }

            // 检查地址冲突
            const conflictCheck = checkAddressConflict(
                config.registers || [],
                {
                    registerType: values.registerType,
                    address: values.address,
                    quantity: actualQuantity,
                },
                mode === 'edit' ? current?.id : undefined
            );

            if (conflictCheck.conflict) {
                const reg = conflictCheck.conflictWith;
                if (!reg) return;
                Modal.error({
                    title: '地址冲突',
                    content: `与寄存器「${reg.name}」地址范围冲突 (地址 ${reg.address}-${reg.address + reg.quantity - 1})`,
                });
                return;
            }

            // 构建 dictConfig（Bool 值映射；非 BOOL 类型保留已有配置）
            let dictConfig: ModbusDictConfig | undefined;
            if (values.dataType === 'BOOL') {
                dictConfig =
                    values.boolLabel0 || values.boolLabel1
                        ? {
                              items: [
                                  ...(values.boolLabel0
                                      ? [{ key: '0', label: values.boolLabel0 }]
                                      : []),
                                  ...(values.boolLabel1
                                      ? [{ key: '1', label: values.boolLabel1 }]
                                      : []),
                              ],
                          }
                        : undefined;
            } else {
                // 非 BOOL 类型：编辑时保留已有 dictConfig，创建时无
                dictConfig = mode === 'edit' ? current?.dictConfig : undefined;
            }

            // 仅 COIL/HOLDING_REGISTER 可配置 writable，其他类型始终 false
            const isWritableType =
                values.registerType === 'COIL' || values.registerType === 'HOLDING_REGISTER';
            const writable = isWritableType ? !!values.writable : false;
            const inputScale = Number(values.scale);
            const scale =
                values.registerType === 'COIL' || values.registerType === 'DISCRETE_INPUT'
                    ? 1
                    : Number.isFinite(inputScale) && inputScale > 0
                      ? inputScale
                      : 1;
            const group = normalizeGroupName(values.group);

            const registerFields = {
                name: values.name,
                group: group || undefined,
                registerType: values.registerType,
                address: values.address,
                dataType: values.dataType,
                byteOrder:
                    values.registerType === 'HOLDING_REGISTER' ||
                    values.registerType === 'INPUT_REGISTER'
                        ? values.byteOrder || undefined
                        : undefined,
                quantity: actualQuantity,
                writable,
                unit: values.unit,
                scale,
                decimals: values.decimals,
                dictConfig,
                remark: values.remark,
            };

            let newRegisters: Modbus.Register[];

            if (mode === 'create') {
                newRegisters = [
                    ...(config.registers || []),
                    { id: generateId(), ...registerFields },
                ];
            } else {
                newRegisters = config.registers.map((r) =>
                    r.id === current?.id ? { ...r, ...registerFields } : r
                );
            }

            await saveMutation.mutateAsync({
                id: typeId,
                protocol: 'Modbus',
                config: {
                    byteOrder: config.byteOrder,
                    readInterval: config.readInterval,
                    packet: normalizePacketConfig(config.packet),
                    registers: newRegisters,
                },
            });

            onSuccess?.();
            setOpen(false);
        };

        // 线圈/离散输入只支持 BOOL；保持寄存器/输入寄存器不支持 BOOL
        const isBitRegister = registerType === 'COIL' || registerType === 'DISCRETE_INPUT';
        const isWordRegister =
            registerType === 'HOLDING_REGISTER' || registerType === 'INPUT_REGISTER';

        return (
            <FormModal
                title={mode === 'create' ? '新增寄存器' : '编辑寄存器'}
                open={open}
                onOk={handleOk}
                onCancel={() => setOpen(false)}
                confirmLoading={saveMutation.isPending}
                forceRender
            >
                <Form form={form} layout="vertical">
                    <Form.Item
                        label="名称"
                        name="name"
                        rules={[{ required: true, message: '请输入名称' }]}
                    >
                        <Input placeholder="如：温度、湿度、电压" />
                    </Form.Item>

                    <Form.Item
                        label="分组"
                        name="group"
                        extra="同一分组的寄存器会在配置页聚合为同一组卡片，留空则显示在未分组中"
                    >
                        <AutoComplete
                            allowClear
                            options={groupOptions.options}
                            placeholder="例如：基础信息、告警、控制"
                            filterOption={false}
                            onDropdownVisibleChange={groupOptions.onDropdownVisibleChange}
                            onSearch={groupOptions.onSearch}
                        />
                    </Form.Item>

                    <Flex gap={16}>
                        <Form.Item
                            label="寄存器类型"
                            name="registerType"
                            rules={[{ required: true, message: '请选择寄存器类型' }]}
                            className="flex-1"
                        >
                            <Select
                                options={RegisterTypeOptions}
                                onChange={(val) => {
                                    if (val === 'COIL' || val === 'DISCRETE_INPUT') {
                                        form.setFieldsValue({
                                            dataType: 'BOOL',
                                            decimals: undefined,
                                            writable: false,
                                            scale: 1,
                                            byteOrder: undefined,
                                        });
                                    } else if (form.getFieldValue('dataType') === 'BOOL') {
                                        form.setFieldsValue({
                                            dataType: 'INT16',
                                            decimals: undefined,
                                            boolLabel0: undefined,
                                            boolLabel1: undefined,
                                        });
                                    }
                                    // 切换到只读类型时清除 writable
                                    if (val === 'DISCRETE_INPUT' || val === 'INPUT_REGISTER') {
                                        form.setFieldsValue({ writable: false });
                                    }
                                }}
                            />
                        </Form.Item>
                        <Form.Item
                            label="地址"
                            name="address"
                            rules={[{ required: true, message: '请输入地址' }]}
                            className="flex-1"
                        >
                            <InputNumber
                                min={0}
                                max={65535}
                                className="!w-full"
                                placeholder="0-65535"
                            />
                        </Form.Item>
                    </Flex>

                    <Form.Item
                        label="数据类型"
                        name="dataType"
                        rules={[{ required: true, message: '请选择数据类型' }]}
                    >
                        <Select
                            options={
                                isBitRegister
                                    ? [
                                          {
                                              value: 'BOOL' as const,
                                              label: 'BOOL (1 bit)',
                                              quantity: 1,
                                          },
                                      ]
                                    : isWordRegister
                                      ? DataTypeOptions.filter((o) => o.value !== 'BOOL')
                                      : DataTypeOptions
                            }
                            disabled={isBitRegister}
                            onChange={() => {
                                form.setFieldsValue({
                                    decimals: undefined,
                                    boolLabel0: undefined,
                                    boolLabel1: undefined,
                                });
                            }}
                        />
                    </Form.Item>

                    {isWordRegister && (
                        <Form.Item
                            label="字节序"
                            name="byteOrder"
                            extra="留空时继承设备配置；选择后仅覆盖当前寄存器的读写字节序"
                        >
                            <Select
                                allowClear
                                options={ByteOrderOptions}
                                placeholder="继承设备配置"
                            />
                        </Form.Item>
                    )}

                    {dataType === 'BOOL' && (
                        <Flex gap={16}>
                            <Form.Item label="0 值显示" name="boolLabel0" className="flex-1">
                                <Input placeholder="如：关闭、OFF" />
                            </Form.Item>
                            <Form.Item label="1 值显示" name="boolLabel1" className="flex-1">
                                <Input placeholder="如：开启、ON" />
                            </Form.Item>
                        </Flex>
                    )}

                    <Flex gap={16}>
                        <Form.Item label="单位" name="unit" className="flex-1">
                            <Input placeholder="如：V、A、℃、%" />
                        </Form.Item>
                        <Form.Item
                            label="缩放系数"
                            name="scale"
                            className="flex-1"
                            extra="入库值 = 原始值 × 缩放系数（默认 1）"
                            rules={[
                                {
                                    validator: async (_, value) => {
                                        const numericValue = Number(value);
                                        if (!Number.isFinite(numericValue) || numericValue <= 0) {
                                            throw new Error('请输入大于 0 的缩放系数');
                                        }
                                    },
                                },
                            ]}
                        >
                            <InputNumber
                                min={0.000001 as number}
                                max={1000000 as number}
                                step={0.1}
                                precision={6}
                                formatter={(value) => formatScaleValue(value)}
                                parser={(value) => Number(value || 1)}
                                className="!w-full"
                                disabled={isBitRegister}
                            />
                        </Form.Item>
                        {(dataType === 'FLOAT32' || dataType === 'DOUBLE') && (
                            <Form.Item label="小数位数" name="decimals" className="flex-1">
                                <InputNumber
                                    min={0}
                                    max={8}
                                    placeholder="不限制"
                                    className="!w-full"
                                />
                            </Form.Item>
                        )}
                    </Flex>

                    {(registerType === 'COIL' || registerType === 'HOLDING_REGISTER') && (
                        <Form.Item
                            label="可写"
                            name="writable"
                            valuePropName="checked"
                            extra="开启后该寄存器可用于写操作下发"
                        >
                            <Switch />
                        </Form.Item>
                    )}

                    <Form.Item label="备注" name="remark">
                        <Input placeholder="备注信息" />
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);

export default ModbusConfigPage;
