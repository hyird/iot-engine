/**
 * Modbus 协议配置 - 纯函数、常量与类型助手（从 ModbusConfig 抽离）
 */

import { buildGroupSections } from '../grouping';
import type { Modbus, Protocol } from '../protocol.types';

/** 寄存器类型选项 */
export const RegisterTypeOptions: { value: Modbus.RegisterType; label: string }[] = [
    { value: 'COIL', label: '0X - 线圈 (Coil)' },
    { value: 'DISCRETE_INPUT', label: '1X - 离散输入 (Discrete Input)' },
    { value: 'INPUT_REGISTER', label: '3X - 输入寄存器 (Input Register)' },
    { value: 'HOLDING_REGISTER', label: '4X - 保持寄存器 (Holding Register)' },
];

/** 数据类型选项 */
export const DataTypeOptions: { value: Modbus.DataType; label: string; quantity: number }[] = [
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
export const ByteOrderOptions: { value: Modbus.ByteOrder; label: string }[] = [
    { value: 'BIG_ENDIAN', label: 'Big-endian' },
    { value: 'LITTLE_ENDIAN', label: 'Little-endian' },
    { value: 'BIG_ENDIAN_BYTE_SWAP', label: 'Big-endian byte swap' },
    { value: 'LITTLE_ENDIAN_BYTE_SWAP', label: 'Little-endian byte swap' },
];

export const DEFAULT_PACKET_MERGE_GAP = 100;
export const DEFAULT_PACKET_MAX_QUANTITY = 125;

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

export const normalizeGroupName = (group?: string) => group?.trim() || '';

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
export interface DeviceTypeModalRef {
    open: (mode: 'create' | 'edit', data?: Protocol.Item) => void;
}

/** 寄存器 Modal Ref */
export interface RegisterModalRef {
    open: (mode: 'create' | 'edit', typeId: string, register?: Modbus.Register) => void;
}
