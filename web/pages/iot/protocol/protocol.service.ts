import type { UseQueryOptions } from '@tanstack/react-query';
import { useQueryClient } from '@tanstack/react-query';
import { App } from 'antd';
import type { CSSProperties } from 'react';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import type { PaginatedResult } from '@/utils/pagination';
import { createQueryKeys } from '@/utils/query';
import * as api from './protocol.api';
import { protocolCreateSchema } from './protocol.schema';
import type { Modbus, Protocol, S7, SL651, StoragePolicy } from './protocol.types';
export const normalizeGroupName = (group?: string) => group?.trim() || '';
export const UNGROUPED_GROUP_KEY = '__ungrouped__';
export const getGroupKey = (group?: string) => normalizeGroupName(group) || UNGROUPED_GROUP_KEY;
export interface GroupSection<T> {
    key: string;
    label: string;
    count: number;
    items: T[];
    firstIndex: number;
}
export const buildGroupSections = <
    T extends {
        group?: string;
    },
>(
    items: T[]
): GroupSection<T>[] => {
    const sectionMap = new Map<string, GroupSection<T>>();
    items.forEach((item, index) => {
        const rawGroup = normalizeGroupName(item.group);
        const key = getGroupKey(item.group);
        const label = rawGroup || '未分组';
        const current = sectionMap.get(key);
        if (current) {
            current.count++;
            current.items.push(item);
            return;
        }
        sectionMap.set(key, {
            key,
            label,
            count: 1,
            items: [item],
            firstIndex: index,
        });
    });
    return Array.from(sectionMap.values()).sort((a, b) => {
        if (a.firstIndex !== b.firstIndex) return a.firstIndex - b.firstIndex;
        return a.label.localeCompare(b.label, 'zh-Hans-CN');
    });
};
export const sortSectionsByOrder = <
    T extends {
        key: string;
    },
>(
    sections: T[],
    orderedKeys: string[]
): T[] => {
    const orderIndex = new Map<string, number>(orderedKeys.map((key, index) => [key, index]));
    const originalIndex = new Map<string, number>(
        sections.map((section, index) => [section.key, index])
    );
    return [...sections].sort((left, right) => {
        const leftOrder = orderIndex.get(left.key);
        const rightOrder = orderIndex.get(right.key);
        if (leftOrder !== undefined && rightOrder !== undefined) {
            return leftOrder - rightOrder;
        }
        if (leftOrder !== undefined) return -1;
        if (rightOrder !== undefined) return 1;
        return (originalIndex.get(left.key) ?? 0) - (originalIndex.get(right.key) ?? 0);
    });
};
export const reorderItemsByGroupOrder = <
    T extends {
        group?: string;
    },
>(
    items: T[],
    orderedKeys: string[]
): T[] => {
    const groupMap = new Map<string, T[]>();
    const originalOrder: string[] = [];
    for (const item of items) {
        const key = getGroupKey(item.group);
        const bucket = groupMap.get(key);
        if (bucket) {
            bucket.push(item);
            continue;
        }
        groupMap.set(key, [item]);
        originalOrder.push(key);
    }
    const nextOrder = [
        ...orderedKeys.filter((key) => groupMap.has(key)),
        ...originalOrder.filter((key) => !orderedKeys.includes(key)),
    ];
    return nextOrder.flatMap((key) => groupMap.get(key) ?? []);
};
export const reorderItemsWithinGroupOrder = <
    T extends {
        id: string;
        group?: string;
    },
>(
    items: T[],
    groupKey: string,
    orderedIds: string[]
): T[] => {
    const sections = buildGroupSections(items);
    const orderedIdSet = new Set(orderedIds);
    return sections.flatMap((section) => {
        if (section.key !== groupKey) {
            return section.items;
        }
        const itemMap = new Map(section.items.map((item) => [item.id, item]));
        const nextItems: T[] = [];
        for (const id of orderedIds) {
            const item = itemMap.get(id);
            if (item) {
                nextItems.push(item);
            }
        }
        for (const item of section.items) {
            if (!orderedIdSet.has(item.id)) {
                nextItems.push(item);
            }
        }
        return nextItems;
    });
};

export const protocolQueryKeys = {
    ...createQueryKeys('protocol-configs'),
    list: (params?: Protocol.Query) => ['protocol-configs', 'list', params] as const,
};
export type SaveProtocolConfigParams =
    | (Protocol.CreateDto & {
          id?: undefined;
      })
    | (Protocol.UpdateDto & {
          id: string;
          protocol?: Protocol.Type;
      });
export function useProtocolConfigList(
    params?: Protocol.Query,
    options?: Omit<UseQueryOptions<Protocol.Item[]>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: protocolQueryKeys.list(params),
        queryFn: () => api.getAll(params),
        ...options,
    });
}
export function useProtocolConfigOptions(
    protocol: Protocol.Type,
    options?: Omit<UseQueryOptions<PaginatedResult<Protocol.Option>>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: [...protocolQueryKeys.all, 'options', protocol],
        queryFn: () => api.getOptions(protocol),
        ...options,
    });
}
export function useProtocolConfigDetail(
    id: string | undefined,
    options?: Omit<UseQueryOptions<Protocol.Item>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: protocolQueryKeys.detail(id ?? ''),
        queryFn: () => api.getDetail(id as string),
        enabled: Boolean(id),
        ...options,
    });
}
export function useProtocolConfigSave() {
    return useSaveMutation<SaveProtocolConfigParams, Protocol.CreateDto, Protocol.UpdateDto>({
        createFn: api.create,
        updateFn: api.update,
        toUpdatePayload: (data) => {
            const { id: _id, protocol: _protocol, ...rest } = data;
            return rest as Protocol.UpdateDto;
        },
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [protocolQueryKeys.all],
    });
}
export function useProtocolConfigDelete() {
    return useMutationWithMessage({
        mutationFn: api.remove,
        successMessage: '删除成功',
        invalidateKeys: [protocolQueryKeys.all],
    });
}

export interface GroupOption {
    value: string;
}
export const useFilterableGroupOptions = (groups: string[]) => {
    const [searchText, setSearchText] = useState('');
    const [showAllOnOpen, setShowAllOnOpen] = useState(false);
    const options = useMemo(
        () =>
            groups
                .map((value) => value.trim())
                .filter(Boolean)
                .sort((a, b) => a.localeCompare(b, 'zh-Hans-CN'))
                .map((value) => ({ value })),
        [groups]
    );
    const filteredOptions = useMemo(() => {
        if (showAllOnOpen || !searchText.trim()) {
            return options;
        }
        const keyword = searchText.trim().toLowerCase();
        return options.filter((option) => option.value.toLowerCase().includes(keyword));
    }, [options, searchText, showAllOnOpen]);
    return {
        options: filteredOptions,
        onDropdownVisibleChange: (open: boolean) => {
            setShowAllOnOpen(open);
            if (!open) {
                setSearchText('');
            }
        },
        onSearch: (value: string) => {
            setSearchText(value);
            setShowAllOnOpen(false);
        },
    };
};

/**
 * 协议配置导入导出 Hook
 * 支持 SL651、Modbus 和 S7 配置的 JSON 导入导出
 */
const MAX_NAME_LENGTH = 64;
/** 导出配置项（不含 id/时间戳） */
interface ExportItem {
    protocol: Protocol.Type;
    name: string;
    enabled: boolean;
    config: Protocol.Item['config'];
    remark?: string;
}
/** 导入结果 */
interface ImportResult {
    total: number;
    success: number;
    renamed: string[];
    failed: {
        name: string;
        reason: string;
    }[];
}
/** 生成不冲突的名称 */
function resolveNameConflict(name: string, existingNames: Set<string>): string {
    if (!existingNames.has(name)) return name;
    let index = 1;
    while (true) {
        const suffix = index === 1 ? ' (导入)' : ` (导入) ${index}`;
        const baseName = name.slice(0, Math.max(0, MAX_NAME_LENGTH - suffix.length)).trimEnd();
        const candidate = `${baseName}${suffix}`;
        if (!existingNames.has(candidate)) return candidate;
        index++;
    }
}
export function useProtocolImportExport(protocol: Protocol.Type) {
    const { message } = App.useApp();
    const queryClient = useQueryClient();
    const fileInputRef = useRef<HTMLInputElement | null>(null);
    const [exporting, setExporting] = useState(false);
    const [importing, setImporting] = useState(false);
    /** 导出当前协议的所有配置 */
    const exportConfigs = useCallback(async () => {
        setExporting(true);
        try {
            const configs = await api.getAll({ protocol }, { _silent: true });
            if (!configs.length) {
                message.warning('没有可导出的配置');
                return;
            }
            const exportData: ExportItem[] = configs.map(
                ({ id: _id, created_at: _c, updated_at: _u, ...rest }) => rest
            );
            const json = JSON.stringify(exportData, null, 2);
            const blob = new Blob([json], { type: 'application/json' });
            const url = URL.createObjectURL(blob);
            const date = new Date().toISOString().slice(0, 10).replace(/-/g, '');
            const a = document.createElement('a');
            a.href = url;
            a.download = `${protocol}_configs_${date}.json`;
            a.style.display = 'none';
            document.body.appendChild(a);
            a.click();
            a.remove();
            window.setTimeout(() => URL.revokeObjectURL(url), 1000);
            message.success(`已导出 ${exportData.length} 条配置`);
        } catch (error) {
            const reason = error instanceof Error ? error.message : '未知错误';
            message.error(`导出失败：${reason}`);
        } finally {
            setExporting(false);
        }
    }, [protocol, message]);
    /** 处理导入文件 */
    const processImport = useCallback(
        async (file: File) => {
            setImporting(true);
            try {
                const text = await file.text();
                let rawItems: unknown;
                try {
                    rawItems = JSON.parse(text);
                } catch {
                    message.error('JSON 格式错误');
                    return;
                }
                if (!Array.isArray(rawItems) || rawItems.length === 0) {
                    message.error('文件内容为空或格式不正确');
                    return;
                }
                const items: Protocol.CreateDto[] = [];
                for (let i = 0; i < rawItems.length; i++) {
                    const rawItem = rawItems[i];
                    if (typeof rawItem !== 'object' || rawItem === null || Array.isArray(rawItem)) {
                        message.error(`第 ${i + 1} 项必须是对象`);
                        return;
                    }
                    const itemProtocol = (rawItem as Record<string, unknown>).protocol;
                    if (itemProtocol !== protocol) {
                        const actualProtocol =
                            typeof itemProtocol === 'string' ? itemProtocol : '未指定';
                        message.error(
                            `第 ${i + 1} 项协议类型为 ${actualProtocol}，不能导入到 ${protocol} 页面`
                        );
                        return;
                    }
                    const parsedItem = protocolCreateSchema.safeParse(rawItem);
                    if (!parsedItem.success) {
                        const issue = parsedItem.error.issues[0];
                        const path = issue.path.length
                            ? `${issue.path.map(String).join('.')}：`
                            : '';
                        message.error(`第 ${i + 1} 项 ${path}${issue.message}`);
                        return;
                    }
                    items.push(parsedItem.data);
                }
                // 数据库按全协议范围约束名称唯一，必须加载全部协议名称后再处理冲突。
                const existingList = await api.getAll(undefined, { _silent: true });
                const existingNames = new Set(existingList.map((config) => config.name));
                const result: ImportResult = {
                    total: items.length,
                    success: 0,
                    renamed: [],
                    failed: [],
                };
                for (const item of items) {
                    const finalName = resolveNameConflict(item.name, existingNames);
                    if (finalName !== item.name) {
                        result.renamed.push(`${item.name} → ${finalName}`);
                    }
                    try {
                        await api.create(
                            {
                                ...item,
                                protocol,
                                name: finalName,
                            },
                            { _silent: true }
                        );
                        existingNames.add(finalName);
                        result.success++;
                    } catch (e) {
                        result.failed.push({
                            name: item.name,
                            reason: e instanceof Error ? e.message : '未知错误',
                        });
                    }
                }
                // 刷新缓存
                await queryClient.invalidateQueries({ queryKey: protocolQueryKeys.all });
                // 显示结果
                if (result.success === result.total) {
                    const renameInfo =
                        result.renamed.length > 0 ? `\n重命名：${result.renamed.join('、')}` : '';
                    message.success(`成功导入 ${result.success} 条配置${renameInfo}`);
                } else {
                    const failInfo = result.failed.map((f) => `${f.name}(${f.reason})`).join('、');
                    message.warning(
                        `导入完成：${result.success}/${result.total} 成功${failInfo ? `，失败：${failInfo}` : ''}`
                    );
                }
            } catch (error) {
                const reason = error instanceof Error ? error.message : '未知错误';
                message.error(`导入失败：${reason}`);
            } finally {
                setImporting(false);
                // 重置文件输入，允许再次选择同一文件
                if (fileInputRef.current) fileInputRef.current.value = '';
            }
        },
        [protocol, message, queryClient]
    );
    /** 触发文件选择 */
    const triggerImport = useCallback(() => {
        if (!fileInputRef.current) {
            const input = document.createElement('input');
            input.type = 'file';
            input.accept = '.json';
            input.style.display = 'none';
            document.body.appendChild(input);
            fileInputRef.current = input;
        }
        fileInputRef.current.onchange = (event) => {
            const file = (event.target as HTMLInputElement).files?.[0];
            if (file) processImport(file);
        };
        fileInputRef.current.click();
    }, [processImport]);
    // 组件卸载时清理动态创建的 input 元素
    useEffect(() => {
        return () => {
            if (fileInputRef.current) {
                fileInputRef.current.remove();
                fileInputRef.current = null;
            }
        };
    }, []);
    return { exportConfigs, triggerImport, exporting, importing };
}

/**
 * Modbus 协议配置
 */
/** 寄存器类型选项 */
export const RegisterTypeOptions: {
    value: Modbus.RegisterType;
    label: string;
}[] = [
    { value: 'COIL', label: '0X - 线圈 (Coil)' },
    { value: 'DISCRETE_INPUT', label: '1X - 离散输入 (Discrete Input)' },
    { value: 'INPUT_REGISTER', label: '3X - 输入寄存器 (Input Register)' },
    { value: 'HOLDING_REGISTER', label: '4X - 保持寄存器 (Holding Register)' },
];
/** 数据类型选项 */
export const DataTypeOptions: {
    value: Modbus.DataType;
    label: string;
    quantity: number;
}[] = [
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
export const ByteOrderOptions: {
    value: Modbus.ByteOrder;
    label: string;
}[] = [
    { value: 'BIG_ENDIAN', label: 'Big-endian' },
    { value: 'LITTLE_ENDIAN', label: 'Little-endian' },
    { value: 'BIG_ENDIAN_BYTE_SWAP', label: 'Big-endian byte swap' },
    { value: 'LITTLE_ENDIAN_BYTE_SWAP', label: 'Little-endian byte swap' },
];
export const DEFAULT_PACKET_MERGE_GAP = 100;
export const DEFAULT_PACKET_MAX_QUANTITY = 125;
export const numberOrDefault = (value: unknown, fallback: number) => {
    const numericValue = Number(value);
    return Number.isFinite(numericValue) ? numericValue : fallback;
};
/** 设备类型表单的默认值，也用于兼容缺少新字段的历史配置。 */
export const getDeviceTypeFormValues = (data?: Protocol.Item) => {
    const config = data?.config as Modbus.Config | undefined;
    const packet = normalizePacketConfig(config?.packet);
    return {
        name: data?.name ?? '',
        enabled: data?.enabled ?? true,
        byteOrder: config?.byteOrder ?? 'BIG_ENDIAN',
        readInterval: numberOrDefault(config?.readInterval, 1),
        storagePolicy: config?.storagePolicy ?? 'report',
        commandFastReadDuration: numberOrDefault(config?.commandFastReadDuration, 60),
        commandFastReadInterval: numberOrDefault(config?.commandFastReadInterval, 1),
        packetMergeGap: packet.mergeGap,
        packetMaxQuantity: packet.maxQuantity,
        remark: data?.remark ?? '',
    };
};
export const pairedFormItemClassName = 'min-w-0 flex-1';
export const numericInputClassName = 'min-w-0 flex-1';
export const numericUnitClassName = 'pointer-events-none !w-20 text-center';
export const formatScaleValue = (value: number | string | undefined | null) => {
    if (value === null || value === undefined || value === '') return '';
    const numericValue = Number(value);
    if (!Number.isFinite(numericValue)) return String(value);
    return numericValue
        .toFixed(6)
        .replace(/(\.\d*?)0+$/, '$1')
        .replace(/\.$/, '');
};
export const normalizePacketConfig = (
    packet?: Modbus.PacketConfig
): Required<Modbus.PacketConfig> => {
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
export const REGISTER_TYPE_ORDER: Modbus.RegisterType[] = [
    'COIL',
    'DISCRETE_INPUT',
    'INPUT_REGISTER',
    'HOLDING_REGISTER',
];
export const REGISTER_TYPE_META: Record<
    Modbus.RegisterType,
    {
        label: string;
        color: string;
        prefix: string;
        short: string;
    }
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
const UNKNOWN_REGISTER_TYPE_META = {
    label: '未知寄存器类型',
    color: 'default',
    prefix: '',
    short: '?',
};
/**
 * 将南桥旧配置中的寄存器类型归一化为管理端/API 使用的标准枚举。
 * 未识别值保留给兜底展示，避免一条历史脏数据导致整个配置页崩溃。
 */
export const normalizeRegisterType = (value: unknown): Modbus.RegisterType | undefined => {
    if (typeof value !== 'string') return undefined;
    switch (value.trim().toUpperCase()) {
        case 'COIL':
            return 'COIL';
        case 'DISCRETE':
        case 'DISCRETE_INPUT':
            return 'DISCRETE_INPUT';
        case 'INPUT':
        case 'INPUT_REGISTER':
            return 'INPUT_REGISTER';
        case 'HOLDING':
        case 'HOLDING_REGISTER':
            return 'HOLDING_REGISTER';
        default:
            return undefined;
    }
};
export const getRegisterTypeMeta = (value: unknown) => {
    const registerType = normalizeRegisterType(value);
    if (registerType) return REGISTER_TYPE_META[registerType];
    const suffix = typeof value === 'string' && value.trim() ? `（${value.trim()}）` : '';
    return { ...UNKNOWN_REGISTER_TYPE_META, label: `${UNKNOWN_REGISTER_TYPE_META.label}${suffix}` };
};
export const normalizeModbusRegisters = (registers: unknown): Modbus.Register[] => {
    if (!Array.isArray(registers)) return [];
    return registers
        .filter(
            (register): register is Record<string, unknown> =>
                !!register && typeof register === 'object' && !Array.isArray(register)
        )
        .map((value) => {
            const register = value as unknown as Modbus.Register;
            const registerType = normalizeRegisterType(value.registerType);
            return registerType ? { ...register, registerType } : register;
        });
};
export const REGISTER_CARD_GRID_STYLE = {
    gridTemplateColumns: 'repeat(auto-fill, minmax(240px, 1fr))',
};
export const ModbusConfigurationNormalizeGroupName = (group?: string) => group?.trim() || '';
export interface RegisterGroupSection {
    key: string;
    label: string;
    count: number;
    registers: Modbus.Register[];
    typeCounts: Record<Modbus.RegisterType, number>;
}
export const buildRegisterGroupSections = (
    registers: Modbus.Register[]
): RegisterGroupSection[] => {
    return buildGroupSections(normalizeModbusRegisters(registers)).map((section) => {
        const typeCounts: Record<Modbus.RegisterType, number> = {
            COIL: 0,
            DISCRETE_INPUT: 0,
            HOLDING_REGISTER: 0,
            INPUT_REGISTER: 0,
        };
        for (const register of section.items) {
            const registerType = normalizeRegisterType(register.registerType);
            if (registerType) typeCounts[registerType]++;
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
export const generateId = (): string =>
    '10000000-1000-4000-8000-100000000000'.replace(/[018]/g, (c) =>
        (+c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
    );
/** 根据数据类型获取寄存器数量 */
export const getQuantityByDataType = (dataType: Modbus.DataType): number => {
    const opt = DataTypeOptions.find((o) => o.value === dataType);
    return opt?.quantity ?? 1;
};
/** 检查寄存器地址是否冲突 */
export const checkAddressConflict = (
    registers: Modbus.Register[],
    newRegister: {
        registerType: Modbus.RegisterType;
        address: number;
        quantity: number;
    },
    excludeId?: string
): {
    conflict: boolean;
    conflictWith?: Modbus.Register;
} => {
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
export interface DeviceTypeModalRef {
    open: (mode: 'create' | 'edit', data?: Protocol.Item) => void;
}
/** 寄存器 Modal Ref */
export interface RegisterModalRef {
    open: (mode: 'create' | 'edit', typeId: string, register?: Modbus.Register) => void;
}

/**
 * S7 协议配置
 */
export type DeviceTypeFormValues = {
    deviceType: string;
    plcModel: S7.PlcModel;
    connectionMode: S7.ConnectionMode;
    connectionType: S7.ConnectionType;
    rack: number;
    slot: number;
    localTSAP: string;
    remoteTSAP: string;
    probeMode: S7.ProbeMode;
    handshakeTimeout: number;
    directProbeTimeout: number;
    readInterval: number;
    storagePolicy: StoragePolicy;
    commandFastReadDuration: number;
    commandFastReadInterval: number;
    enabled: boolean;
    remark?: string;
};
export type PlcConnectionPreset = {
    value: S7.PlcModel;
    label: string;
    mode: S7.ConnectionMode;
    rack: number;
    slot: number;
    localTSAP: string;
    remoteTSAP: string;
};
/** 生成唯一 ID（兼容非安全上下文） */
export const S7ConfigurationGenerateId = (): string =>
    '10000000-1000-4000-8000-100000000000'.replace(/[018]/g, (c) =>
        (+c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
    );
export const defaultConfig = (): S7.Config => ({
    deviceType: '',
    plcModel: 'S7-1200',
    connection: {
        mode: 'RACK_SLOT',
        rack: 0,
        slot: 1,
        connectionType: 'PG',
        probeMode: 'STANDARD',
        handshakeTimeout: 5000,
        directProbeTimeout: 5000,
    },
    readInterval: 5,
    storagePolicy: 'report',
    commandFastReadDuration: 60,
    commandFastReadInterval: 1,
    areas: [],
});
export const plcModelOptions: PlcConnectionPreset[] = [
    {
        value: 'S7-200',
        label: 'S7-200',
        mode: 'TSAP',
        rack: 0,
        slot: 1,
        localTSAP: '4D57',
        remoteTSAP: '4D57',
    },
    {
        value: 'S7-300',
        label: 'S7-300',
        mode: 'RACK_SLOT',
        rack: 0,
        slot: 2,
        localTSAP: '0100',
        remoteTSAP: '0102',
    },
    {
        value: 'S7-400',
        label: 'S7-400',
        mode: 'RACK_SLOT',
        rack: 0,
        slot: 3,
        localTSAP: '0100',
        remoteTSAP: '0103',
    },
    {
        value: 'S7-1200',
        label: 'S7-1200',
        mode: 'RACK_SLOT',
        rack: 0,
        slot: 1,
        localTSAP: '0100',
        remoteTSAP: '0101',
    },
    {
        value: 'S7-1500',
        label: 'S7-1500',
        mode: 'RACK_SLOT',
        rack: 0,
        slot: 1,
        localTSAP: '0100',
        remoteTSAP: '0101',
    },
];
export const getPlcPreset = (plcModel: S7.PlcModel) =>
    plcModelOptions.find((option) => option.value === plcModel) ??
    plcModelOptions.find((option) => option.value === 'S7-1200') ??
    plcModelOptions[0];
export const normalizeTsapValue = (value?: string) => {
    if (!value) return undefined;
    const normalized = value
        .replace(/^0x/i, '')
        .replace(/[\s.:\-_]/g, '')
        .toUpperCase();
    return normalized || undefined;
};
export const formatTsapValue = (value?: string) => {
    const normalized = normalizeTsapValue(value);
    if (!normalized || !/^[0-9A-F]{1,4}$/.test(normalized)) {
        return undefined;
    }
    return normalized.padStart(4, '0');
};
export const getConnectionTypeCode = (connectionType?: S7.ConnectionType) => {
    if (connectionType === 'OP') return 0x02;
    if (connectionType === 'S7_BASIC') return 0x03;
    return 0x01;
};
export const buildRemoteTsapFromRackSlot = (
    rack: number,
    slot: number,
    connectionType?: S7.ConnectionType
) =>
    ((getConnectionTypeCode(connectionType) << 8) + rack * 0x20 + slot)
        .toString(16)
        .toUpperCase()
        .padStart(4, '0');
export const inferConnectionMode = (
    plcModel: S7.PlcModel,
    connection?: S7.Connection
): S7.ConnectionMode => {
    if (plcModel === 'S7-200') {
        return 'TSAP';
    }
    if (connection?.mode === 'TSAP' || connection?.mode === 'RACK_SLOT') {
        return connection.mode;
    }
    if (connection?.localTSAP || connection?.remoteTSAP) {
        return 'TSAP';
    }
    return getPlcPreset(plcModel).mode;
};
export const getConnectionFormValues = (plcModel: S7.PlcModel, connection?: S7.Connection) => {
    const preset = getPlcPreset(plcModel);
    const connectionMode = inferConnectionMode(plcModel, connection);
    const connectionType = connection?.connectionType ?? 'PG';
    const rack = connection?.rack ?? preset.rack;
    const slot = connection?.slot ?? preset.slot;
    const localTSAP =
        formatTsapValue(connection?.localTSAP) ??
        (connectionMode === 'TSAP' && preset.mode === 'TSAP' ? preset.localTSAP : '0100');
    const remoteTSAP =
        formatTsapValue(connection?.remoteTSAP) ??
        (connectionMode === 'TSAP' && preset.mode === 'TSAP'
            ? preset.remoteTSAP
            : buildRemoteTsapFromRackSlot(rack, slot, connectionType));
    return {
        connectionMode,
        connectionType,
        rack,
        slot,
        localTSAP,
        remoteTSAP,
        probeMode: connection?.probeMode ?? 'STANDARD',
        handshakeTimeout: connection?.handshakeTimeout ?? 5000,
        directProbeTimeout: connection?.directProbeTimeout ?? 5000,
    };
};
/** 设备类型表单默认值与编辑回填值。 */
export const S7ConfigurationGetDeviceTypeFormValues = (
    data?: Protocol.Item
): DeviceTypeFormValues => {
    const config = data?.config as S7.Config | undefined;
    const plcModel = config?.plcModel ?? 'S7-1200';
    const numberOrDefault = (value: unknown, fallback: number) => {
        const numericValue = Number(value);
        return Number.isFinite(numericValue) ? numericValue : fallback;
    };
    return {
        deviceType: data?.name ?? '',
        plcModel,
        ...getConnectionFormValues(plcModel, config?.connection),
        readInterval: numberOrDefault(config?.readInterval, 5),
        storagePolicy: config?.storagePolicy ?? 'report',
        commandFastReadDuration: numberOrDefault(config?.commandFastReadDuration, 60),
        commandFastReadInterval: numberOrDefault(config?.commandFastReadInterval, 1),
        enabled: data?.enabled ?? true,
        remark: data?.remark ?? '',
    };
};
export const buildConnectionConfig = (values: DeviceTypeFormValues): S7.Connection => {
    const preset = getPlcPreset(values.plcModel);
    const forceTsap = values.plcModel === 'S7-200' || values.connectionMode === 'TSAP';
    if (forceTsap) {
        return {
            mode: 'TSAP',
            connectionType: values.connectionType,
            rack: undefined,
            slot: undefined,
            localTSAP: formatTsapValue(values.localTSAP) ?? preset.localTSAP,
            remoteTSAP: formatTsapValue(values.remoteTSAP) ?? preset.remoteTSAP,
            probeMode: values.probeMode,
            handshakeTimeout: values.handshakeTimeout,
            directProbeTimeout: values.directProbeTimeout,
        };
    }
    return {
        mode: 'RACK_SLOT',
        connectionType: values.connectionType,
        rack: values.rack,
        slot: values.slot,
        localTSAP: undefined,
        remoteTSAP: undefined,
        probeMode: values.probeMode,
        handshakeTimeout: values.handshakeTimeout,
        directProbeTimeout: values.directProbeTimeout,
    };
};
export const connectionTypeOptions: {
    value: S7.ConnectionType;
    label: string;
}[] = [
    { value: 'PG', label: 'PG' },
    { value: 'OP', label: 'OP' },
    { value: 'S7_BASIC', label: 'S7BASIC' },
];
export const connectionModeOptions: {
    value: S7.ConnectionMode;
    label: string;
}[] = [
    { value: 'RACK_SLOT', label: 'Rack/Slot' },
    { value: 'TSAP', label: 'TSAP' },
];
export const getConnectionModeOptions = (
    plcModel?: S7.PlcModel
): {
    value: S7.ConnectionMode;
    label: string;
}[] => (plcModel === 'S7-200' ? [{ value: 'TSAP', label: 'TSAP' }] : connectionModeOptions);
export const connectionTypeTips: Record<S7.ConnectionType, string> = {
    PG: 'PG（编程）模式，常用于本地接线连接与离线调试',
    OP: 'OP（操作）模式，适合上位机常态采集场景',
    S7_BASIC: 'S7 Basic，兼容部分第三方网关的轻量模式',
};
export const connectionModeTips: Record<S7.ConnectionMode, string> = {
    RACK_SLOT: '常规西门子 PLC 连接方式，适合 S7-300/400/1200/1500 等场景',
    TSAP: '直接指定本地/远端 TSAP，适合 S7-200、CP243 或需要手工指定 TSAP 的场景',
};
export const getConnectionModeTip = (
    plcModel?: S7.PlcModel,
    connectionMode?: S7.ConnectionMode
) => {
    if (plcModel === 'S7-200') {
        return 'S7-200 仅支持 TSAP 连接模式';
    }
    return connectionModeTips[connectionMode || 'RACK_SLOT'];
};
export const getConnectionTypeLabel = (connectionType?: S7.ConnectionType) =>
    connectionTypeOptions.find((item) => item.value === connectionType)?.label || 'PG';
export const getConnectionModeLabel = (connectionMode?: S7.ConnectionMode) =>
    connectionModeOptions.find((item) => item.value === connectionMode)?.label || 'Rack/Slot';
export const normalizeAreaTypeForPlcModel = (
    plcModel?: S7.PlcModel,
    areaType?: S7.AreaType
): S7.AreaType | undefined => {
    if (!areaType) {
        return areaType;
    }
    if (plcModel === 'S7-200' && areaType === 'DB') {
        return 'V';
    }
    return areaType;
};
export const getAreaTypeOptions = (
    plcModel?: S7.PlcModel
): {
    value: S7.AreaType;
    label: string;
}[] => {
    if (plcModel === 'S7-200') {
        return [
            { value: 'V', label: 'V（变量存储器）' },
            { value: 'MK', label: 'M（MK，标记位）' },
            { value: 'PE', label: 'I（PE，系统输入）' },
            { value: 'PA', label: 'Q（PA，系统输出）' },
            { value: 'CT', label: 'C（CT，计数器）' },
            { value: 'TM', label: 'T（TM，定时器）' },
        ];
    }
    return [
        { value: 'DB', label: 'DB（数据块）' },
        { value: 'MK', label: 'M（MK，标记位）' },
        { value: 'PE', label: 'I（PE，系统输入）' },
        { value: 'PA', label: 'Q（PA，系统输出）' },
        { value: 'CT', label: 'C（CT，计数器）' },
        { value: 'TM', label: 'T（TM，定时器）' },
    ];
};
export const validateTsapValue = async (_: unknown, value?: string) => {
    if (!formatTsapValue(value)) {
        throw new Error('请输入 1-4 位十六进制 TSAP，例如 4D57 或 0200');
    }
};
export const areaDataTypeOptions: {
    value: S7.AreaDataType;
    label: string;
}[] = [
    { value: 'BOOL', label: '布尔（BOOL）' },
    { value: 'INT8', label: '8位整型（INT8）' },
    { value: 'UINT8', label: '8位无符号整型（UINT8）' },
    { value: 'INT16', label: '16位有符号整型（INT16）' },
    { value: 'UINT16', label: '16位无符号整型（UINT16）' },
    { value: 'INT32', label: '32位有符号整型（INT32）' },
    { value: 'UINT32', label: '32位无符号整型（UINT32）' },
    { value: 'FLOAT', label: '浮点（FLOAT）' },
    { value: 'LREAL', label: '双精度（LREAL）' },
    { value: 'STRING', label: '字符串（STRING）' },
];
export const areaDataTypeSizeMap: Record<S7.AreaDataType, number> = {
    BOOL: 1,
    INT8: 1,
    UINT8: 1,
    INT16: 2,
    UINT16: 2,
    INT32: 4,
    UINT32: 4,
    FLOAT: 4,
    LREAL: 8,
    STRING: 1,
};
export const getDataTypeSize = (dataType?: S7.AreaDataType) =>
    dataType ? (areaDataTypeSizeMap[dataType] ?? 1) : 1;
export const writableAreaTypes: S7.AreaType[] = ['DB', 'V', 'MK', 'PA'];
export const bitOnlyAreaTypes: S7.AreaType[] = ['PE', 'PA'];
export const AREA_CARD_GRID_STYLE: CSSProperties = {
    gridTemplateColumns: 'repeat(auto-fill, minmax(320px, 1fr))',
};
export const getAreaDataTypeOptions = (
    areaType?: S7.AreaType
): {
    value: S7.AreaDataType;
    label: string;
}[] => {
    if (areaType === 'CT' || areaType === 'TM') {
        return areaDataTypeOptions.filter((item) => item.value === 'UINT16');
    }
    if (bitOnlyAreaTypes.includes(areaType || 'DB')) {
        return areaDataTypeOptions.filter((item) => item.value === 'BOOL');
    }
    return areaDataTypeOptions;
};
export const areaAddressPrefixMap: Record<
    S7.AreaType,
    {
        bool: string;
        number: string;
    }
> = {
    DB: { bool: 'X', number: 'DB' },
    V: { bool: 'V', number: 'V' },
    MK: { bool: 'M', number: 'M' },
    PE: { bool: 'I', number: 'I' },
    PA: { bool: 'Q', number: 'Q' },
    CT: { bool: 'C', number: 'C' },
    TM: { bool: 'T', number: 'T' },
};
export const areaAddressHintMap: Record<S7.AreaType, string> = {
    DB: 'DB1.DBX0.0、DB1.DBW10、DB1.DBD0、DB1.DBD1（LREAL，DBD 为4字节）',
    V: 'V0.0、VB10、VW20、VD30',
    MK: 'M0.0、MB10、MW20、MD30',
    PE: 'I0.0、IB10、IW20、ID30',
    PA: 'Q0.0、QB10、QW20、QD30',
    CT: 'C0、C1',
    TM: 'T0、T1',
};
export const areaAddressTypeMap: Record<S7.AreaDataType, string> = {
    BOOL: 'X',
    INT8: 'B',
    UINT8: 'B',
    INT16: 'W',
    UINT16: 'W',
    INT32: 'D',
    UINT32: 'D',
    FLOAT: 'D',
    LREAL: 'D',
    STRING: 'B',
};
export const normalizeS7DataType = (value?: string): S7.AreaDataType => {
    if (value === 'LREAL' || value === 'DOUBLE') {
        return 'LREAL';
    }
    if (
        value === 'BOOL' ||
        value === 'INT8' ||
        value === 'UINT8' ||
        value === 'INT16' ||
        value === 'UINT16' ||
        value === 'INT32' ||
        value === 'UINT32' ||
        value === 'FLOAT' ||
        value === 'STRING'
    ) {
        return value;
    }
    return 'INT16';
};
export const supportsS7Decimals = (dataType?: S7.AreaDataType) =>
    dataType === 'FLOAT' || dataType === 'LREAL';
export const getAreaAddressSample = (
    areaType?: S7.AreaType,
    dataType?: S7.AreaDataType,
    dbNumber?: number,
    start?: number,
    bit?: number
) => {
    if (!areaType) return '';
    const safeStart = typeof start === 'number' ? start : 0;
    const safeBit = typeof bit === 'number' ? bit : 0;
    const area = areaAddressPrefixMap[areaType];
    const suffix = areaAddressTypeMap[dataType || 'INT16'];
    if (areaType === 'CT' || areaType === 'TM') {
        return `${area.number}${safeStart}`;
    }
    if (areaType === 'DB') {
        if (!dbNumber) return '';
        if (suffix === 'X') return `DB${dbNumber}.DBX${safeStart}.${safeBit}`;
        return `DB${dbNumber}.DB${suffix}${safeStart}`;
    }
    if (areaType === 'V') {
        if (suffix === 'X') return `V${safeStart}.${safeBit}`;
        return `V${suffix}${safeStart}`;
    }
    if (suffix === 'X') return `${area.bool}${safeStart}.${safeBit}`;
    return `${area.number}${suffix}${safeStart}`;
};
export const getAddressSuffixExample = (areaType?: S7.AreaType, dataType?: S7.AreaDataType) => {
    if (!areaType) return '';
    const area = areaAddressPrefixMap[areaType];
    if (areaType === 'CT' || areaType === 'TM') return '';
    const suffix = areaAddressTypeMap[dataType || 'INT16'];
    if (areaType === 'V') {
        if (suffix === 'X') return 'V0.0';
        return `V${suffix}0`;
    }
    if (suffix === 'X') return `${area.bool}X.0`;
    return `${area.number}${suffix}0`;
};
export const supportsBitAddress = (areaType?: S7.AreaType, dataType?: S7.AreaDataType) =>
    dataType === 'BOOL' && !!areaType && areaType !== 'CT' && areaType !== 'TM';
export const getAddressRuleText = (areaType: S7.AreaType | undefined, isBool: boolean) => {
    if (areaType === 'DB') {
        return isBool ? '请输入 DB 位号（如 DB1.DBX0.0）' : '请输入 DB 字节偏移（如 DB1.DBW10）';
    }
    if (areaType === 'V') {
        return isBool ? '请输入 V 位地址（如 V0.0）' : '请输入 V 偏移（如 VB10 / VW20）';
    }
    if (areaType === 'CT') {
        return '请输入计数器地址（如 C0）';
    }
    if (areaType === 'TM') {
        return '请输入定时器地址（如 T0）';
    }
    return isBool ? '请输入位地址（如 M0.0/I0.1/Q1.2）' : '请输入字节偏移';
};
export const getAreaAddressRangeText = (area: S7.Area) => {
    const dataType = normalizeS7DataType(area.dataType);
    const start = typeof area.start === 'number' && area.start >= 0 ? area.start : 0;
    const size =
        typeof area.size === 'number' && area.size > 0 ? area.size : getDataTypeSize(dataType) || 1;
    const end =
        dataType === 'STRING'
            ? start + Math.max(size, 1) - 1
            : dataType === 'LREAL'
              ? start + 1
              : start;
    const startBit = typeof area.startBit === 'number' ? area.startBit : 0;
    const startAddress = getAreaAddressSample(area.area, dataType, area.dbNumber, start, startBit);
    const endAddress = getAreaAddressSample(area.area, dataType, area.dbNumber, end, startBit);
    return {
        start: startAddress || '地址异常',
        end: endAddress || '地址异常',
    };
};

/**
 * SL651 协议配置 - 共享类型和常量
 */
/** 编码类型列表 */
export const EncodeList: SL651.EncodeType[] = ['BCD', 'TIME_YYMMDDHHMMSS', 'JPEG', 'DICT', 'HEX'];
/** 生成唯一 ID（兼容非安全上下文） */
export const Sl651FormGenerateId = (): string =>
    '10000000-1000-4000-8000-100000000000'.replace(/[018]/g, (c) =>
        (+c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
    );
/** SaveMutation 类型（避免每个 Modal 重复定义） */
export type SaveMutation = ReturnType<typeof useProtocolConfigSave>;
/** 设备类型表单默认值与编辑回填值。 */
export const Sl651FormGetDeviceTypeFormValues = (data?: Protocol.Item) => {
    const config = data?.config as SL651.Config | undefined;
    return {
        name: data?.name ?? '',
        enabled: data?.enabled ?? true,
        responseMode: config?.responseMode ?? 'M1',
        storagePolicy: config?.storagePolicy ?? 'report',
        remark: data?.remark ?? '',
    };
};
/** 表单中的条件数据（可能不完整） */
export interface FormCondition {
    bitIndex?: string;
    bitValue?: string;
}
/** 表单中的映射项数据（可能不完整） */
export interface FormMapItem {
    key?: string;
    label?: string;
    value?: string;
    dependsOn?: {
        operator?: 'AND' | 'OR';
        conditions?: FormCondition[];
    };
}

export { getRevisions } from './protocol.api';
