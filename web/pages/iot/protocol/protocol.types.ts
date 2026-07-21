/**
 * 协议配置类型定义
 */

import type { PageParams } from '@/utils/types';

/** 协议类型 */
export type ProtocolType = 'SL651' | 'Modbus' | 'S7';

/** 设备类型采集与存储策略 */
export interface DeviceTypeTimingConfig {
    /** 历史数据存储间隔（秒），默认 1 */
    storageInterval?: number;
    /** 下发后快读窗口（秒），0 表示关闭，默认 60 */
    commandFastReadDuration?: number;
    /** 下发后快读间隔（秒），默认 1 */
    commandFastReadInterval?: number;
}

/** SL651 传输方向 */
export type SL651Direction = 'UP' | 'DOWN';

/** SL651 应答模式 */
export type SL651ResponseMode = 'M1' | 'M2' | 'M3' | 'M4';

/** SL651 编码类型 */
export type SL651EncodeType = 'BCD' | 'TIME_YYMMDDHHMMSS' | 'JPEG' | 'DICT' | 'HEX';

/** SL651 要素预设值选项 */
export interface SL651ElementOption {
    label: string;
    value: string;
}

/** SL651 字典映射类型 */
export type SL651DictMapType = 'VALUE' | 'BIT';

/** SL651 字典映射依赖条件 */
export interface SL651DictDependency {
    /** 依赖的位号 */
    bitIndex: string;
    /** 依赖位的期望值（"0"或"1"） */
    bitValue: string;
}

/** SL651 字典映射项 */
export interface SL651DictMapItem {
    /** 值或位号（VALUE模式下为值，BIT模式下为位号0-31） */
    key: string;
    /** 映射的文本 */
    label: string;
    /** 触发值（仅BIT模式使用，"0"或"1"，表示该位为此值时触发） */
    value?: string;
    /** 依赖条件（仅BIT模式使用，用于指定该位映射需要满足的其他位的条件） */
    dependsOn?: {
        /** 条件运算符：AND表示所有条件都必须满足，OR表示至少一个条件满足 */
        operator: 'AND' | 'OR';
        /** 条件列表 */
        conditions: SL651DictDependency[];
    };
}

/** SL651 字典配置 */
export interface SL651DictConfig {
    /** 映射类型 */
    mapType: SL651DictMapType;
    /** 映射项列表 */
    items: SL651DictMapItem[];
}

/** SL651 要素定义 */
export interface SL651Element {
    id: string;
    name: string;
    /** 分组名称（用于配置页按卡片分组展示） */
    group?: string;
    guideHex: string;
    encode: SL651EncodeType;
    length: number;
    digits: number;
    unit?: string;
    remark?: string;
    /** 预设值选项（仅下行功能码要素使用） */
    options?: SL651ElementOption[];
    /** 字典配置（仅 DICT 编码类型使用） */
    dictConfig?: SL651DictConfig;
}

/** SL651 功能码定义 */
export interface SL651Func {
    id: string;
    funcCode: string;
    dir: SL651Direction;
    name: string;
    remark?: string;
    elements: SL651Element[];
    /** 应答要素（仅下行功能码使用，用于解析设备对下行指令的应答报文） */
    responseElements?: SL651Element[];
}

/** SL651 配置结构 */
export interface SL651Config extends DeviceTypeTimingConfig {
    responseMode?: SL651ResponseMode;
    funcs: SL651Func[];
}

/** Modbus 寄存器类型 */
export type ModbusRegisterType =
    | 'COIL' // 线圈（读01/写05/写多个15）
    | 'DISCRETE_INPUT' // 离散输入（只读02）
    | 'HOLDING_REGISTER' // 保持寄存器（读03/写06/写多个16）
    | 'INPUT_REGISTER'; // 输入寄存器（只读04）

/** Modbus 数据类型 */
export type ModbusDataType =
    | 'BOOL'
    | 'INT16'
    | 'UINT16'
    | 'INT32'
    | 'UINT32'
    | 'FLOAT32'
    | 'INT64'
    | 'UINT64'
    | 'DOUBLE';

/** Modbus 字节序 */
export type ModbusByteOrder =
    | 'BIG_ENDIAN'
    | 'LITTLE_ENDIAN'
    | 'BIG_ENDIAN_BYTE_SWAP'
    | 'LITTLE_ENDIAN_BYTE_SWAP';

/** Modbus 字典映射项 */
export interface ModbusDictMapItem {
    /** 数值 */
    key: string;
    /** 映射文本 */
    label: string;
}

/** Modbus 字典配置 */
export interface ModbusDictConfig {
    /** 映射项列表 */
    items: ModbusDictMapItem[];
}

/** Modbus 寄存器定义 */
export interface ModbusRegister {
    /** 唯一 ID */
    id: string;
    /** 寄存器名称 */
    name: string;
    /** 分组名称（用于配置页按卡片分组展示） */
    group?: string;
    /** 寄存器类型 */
    registerType: ModbusRegisterType;
    /** 寄存器地址（十进制） */
    address: number;
    /** 数据类型 */
    dataType: ModbusDataType;
    /** 字节序；未配置时继承设备配置 */
    byteOrder?: ModbusByteOrder;
    /** 寄存器数量（根据数据类型自动计算） */
    quantity: number;
    /** 单位 */
    unit?: string;
    /** 缩放系数（入库值 = 原始值 * scale，默认 1） */
    scale?: number;
    /** 小数位数（仅 FLOAT32/DOUBLE 生效，-1 或 undefined 表示不限制） */
    decimals?: number;
    /** 是否可写（仅可写寄存器支持下发写操作） */
    writable?: boolean;
    /** 字典配置 */
    dictConfig?: ModbusDictConfig;
    /** 备注 */
    remark?: string;
}

/** Modbus 配置结构 */
export interface ModbusPacketConfig {
    /** 组包地址间隙（两个寄存器地址差 <= 该值时可合并） */
    mergeGap?: number;
    /** 单包最大寄存器数量（字寄存器，1-125） */
    maxQuantity?: number;
}

/** Modbus 配置结构 */
export interface ModbusConfig extends DeviceTypeTimingConfig {
    /** 字节序 */
    byteOrder: ModbusByteOrder;
    /** 读取间隔（秒），默认 1 */
    readInterval?: number;
    /** 组包配置 */
    packet?: ModbusPacketConfig;
    /** 寄存器列表 */
    registers: ModbusRegister[];
}

/** S7 连接类型 */
export type S7ConnectionType = 'PG' | 'OP' | 'S7_BASIC';

/** S7 连接模式 */
export type S7ConnectionMode = 'RACK_SLOT' | 'TSAP';

/** S7 PLC 型号 */
export type S7PlcModel = 'S7-200' | 'S7-300' | 'S7-400' | 'S7-1200' | 'S7-1500';

/** S7 区域类型 */
export type S7AreaType = 'DB' | 'V' | 'MK' | 'PE' | 'PA' | 'CT' | 'TM';

/** S7 数据类型 */
export type S7AreaDataType =
    | 'BOOL'
    | 'INT8'
    | 'UINT8'
    | 'INT16'
    | 'UINT16'
    | 'INT32'
    | 'UINT32'
    | 'FLOAT'
    | 'LREAL'
    | 'STRING';

/** S7 区域定义 */
export interface S7Area {
    id: string;
    name: string;
    /** 分组名称（用于配置页按卡片分组展示） */
    group?: string;
    area: S7AreaType;
    dataType?: S7AreaDataType;
    dbNumber?: number;
    start: number;
    startBit?: number;
    size: number;
    /** 单位 */
    unit?: string;
    /** 小数位数（-1 或 undefined 表示原始值不处理） */
    decimals?: number;
    writable?: boolean;
    remark?: string;
}

/** S7 连接配置（连接地址由链路决定） */
export interface S7Connection {
    mode?: S7ConnectionMode;
    rack?: number;
    slot?: number;
    localTSAP?: string;
    remoteTSAP?: string;
    connectionType?: S7ConnectionType;
}

/** S7 配置结构 */
export interface S7Config extends DeviceTypeTimingConfig {
    deviceType: string;
    plcModel: S7PlcModel;
    connection: S7Connection;
    pollInterval?: number;
    areas: S7Area[];
}

/** 协议配置项 */
export interface ProtocolConfigItem {
    id: string;
    protocol: ProtocolType;
    name: string;
    enabled: boolean;
    config: SL651Config | ModbusConfig | S7Config | Record<string, unknown>;
    remark?: string;
    created_at?: string;
    updated_at?: string;
}

/** 协议配置选项（下拉列表） */
export interface ProtocolConfigOption {
    id: string;
    name: string;
}

/** 协议配置查询参数 */
export interface ProtocolConfigQuery extends PageParams {
    protocol?: ProtocolType;
}

/** 创建协议配置 DTO */
export interface CreateProtocolConfigDto {
    protocol: ProtocolType;
    name: string;
    enabled?: boolean;
    config: SL651Config | ModbusConfig | S7Config | Record<string, unknown>;
    remark?: string;
}

/** 更新协议配置 DTO */
export interface UpdateProtocolConfigDto {
    name?: string;
    enabled?: boolean;
    config?: SL651Config | ModbusConfig | S7Config | Record<string, unknown>;
    remark?: string;
}

/** 协议配置命名空间 */
export namespace Protocol {
    export type Type = ProtocolType;
    export type Item = ProtocolConfigItem;
    export type Option = ProtocolConfigOption;
    export type Query = ProtocolConfigQuery;
    export type CreateDto = CreateProtocolConfigDto;
    export type UpdateDto = UpdateProtocolConfigDto;
}

/** SL651 命名空间 */
export namespace SL651 {
    export type Direction = SL651Direction;
    export type ResponseMode = SL651ResponseMode;
    export type EncodeType = SL651EncodeType;
    export type Element = SL651Element;
    export type Func = SL651Func;
    export type Config = SL651Config;
    export type DictMapType = SL651DictMapType;
    export type DictMapItem = SL651DictMapItem;
    export type DictConfig = SL651DictConfig;
    export type DictDependency = SL651DictDependency;
}

/** Modbus 命名空间 */
export namespace Modbus {
    export type RegisterType = ModbusRegisterType;
    export type DataType = ModbusDataType;
    export type ByteOrder = ModbusByteOrder;
    export type DictMapItem = ModbusDictMapItem;
    export type DictConfig = ModbusDictConfig;
    export type PacketConfig = ModbusPacketConfig;
    export type Register = ModbusRegister;
    export type Config = ModbusConfig;
}

/** S7 命名空间 */
export namespace S7 {
    export type ConnectionMode = S7ConnectionMode;
    export type ConnectionType = S7ConnectionType;
    export type PlcModel = S7PlcModel;
    export type AreaType = S7AreaType;
    export type AreaDataType = S7AreaDataType;
    export type Area = S7Area;
    export type Connection = S7Connection;
    export type Config = S7Config;
}
