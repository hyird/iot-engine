/**
 * S7 协议配置 - 纯函数、常量与类型助手（从 S7Config 抽离）
 */

import type { CSSProperties } from 'react';
import type { S7 } from '../protocol.types';

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
    pollInterval: number;
    storageInterval: number;
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
export const generateId = (): string =>
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
    pollInterval: 5,
    storageInterval: 1,
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

export const connectionTypeOptions: { value: S7.ConnectionType; label: string }[] = [
    { value: 'PG', label: 'PG' },
    { value: 'OP', label: 'OP' },
    { value: 'S7_BASIC', label: 'S7BASIC' },
];

export const connectionModeOptions: { value: S7.ConnectionMode; label: string }[] = [
    { value: 'RACK_SLOT', label: 'Rack/Slot' },
    { value: 'TSAP', label: 'TSAP' },
];

export const getConnectionModeOptions = (
    plcModel?: S7.PlcModel
): { value: S7.ConnectionMode; label: string }[] =>
    plcModel === 'S7-200' ? [{ value: 'TSAP', label: 'TSAP' }] : connectionModeOptions;

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
): { value: S7.AreaType; label: string }[] => {
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

export const areaDataTypeOptions: { value: S7.AreaDataType; label: string }[] = [
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
): { value: S7.AreaDataType; label: string }[] => {
    if (areaType === 'CT' || areaType === 'TM') {
        return areaDataTypeOptions.filter((item) => item.value === 'UINT16');
    }
    if (bitOnlyAreaTypes.includes(areaType || 'DB')) {
        return areaDataTypeOptions.filter((item) => item.value === 'BOOL');
    }
    return areaDataTypeOptions;
};

export const areaAddressPrefixMap: Record<S7.AreaType, { bool: string; number: string }> = {
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
