/**
 * 设备管理类型定义
 */

import type { PageParams } from '@/utils/types';
import type { Link } from '../link/link.types';
import type { Protocol } from '../protocol/protocol.types';

/** 设备状态 */
export type DeviceStatus = 'enabled' | 'disabled';

/** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
export type ModbusMode = 'TCP' | 'RTU';

/** 心跳包模式 */
export type HeartbeatMode = 'OFF' | 'HEX' | 'ASCII';

/** 注册包模式 */
export type RegistrationMode = 'OFF' | 'HEX' | 'ASCII';

/** 设备资源访问级别 */
export type DeviceAccessLevel = 'owner' | 'control' | 'view' | 'none';

/** 设备分享权限 */
export type DeviceSharePermission = 'view' | 'control';

/** 心跳包配置 */
export interface HeartbeatConfig {
    mode: HeartbeatMode;
    content?: string;
}

/** 注册包配置 */
export interface RegistrationConfig {
    mode: RegistrationMode;
    content?: string;
}

/** 设备列表项 */
export interface DeviceItem {
    id: number;
    name: string;
    /** 全局唯一设备编码 */
    device_code: string;
    /** 关联链路 ID（0 表示 Agent 模式） */
    link_id: number;
    /** TCP Client 链路中的目标 ID */
    target_id?: string;
    /** 关联协议配置 ID */
    protocol_config_id: number;
    /** 启用状态 */
    status: DeviceStatus;
    /** 所属分组 ID */
    group_id?: number | null;
    /** 在线超时时间（秒），默认 300 秒（5分钟） */
    online_timeout?: number;
    /** 是否允许远控（下发指令），默认 true */
    remote_control?: boolean;
    /** 读取间隔（秒），来自设备类型配置 */
    read_interval?: number;
    /** 历史数据存储间隔（秒），来自设备类型配置 */
    storage_interval?: number;
    /** 协议配置中的展示要素数量，用于稳定设备卡片布局 */
    element_count?: number;
    /** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
    modbus_mode?: ModbusMode;
    /** Modbus 从站地址（1-247），默认 1 */
    slave_id?: number;
    /** 设备时区（用于报文时间解析，默认 +08:00） */
    timezone?: string;
    /** 心跳包配置 */
    heartbeat?: HeartbeatConfig;
    /** 注册包配置 */
    registration?: RegistrationConfig;
    /** 备注 */
    remark?: string;
    created_by?: number;
    created_at?: string;
    updated_at?: string;

    // 关联数据（列表查询时返回）
    link_name?: string;
    link_mode?: Link.Mode;
    link_protocol?: Link.Protocol;
    protocol_name?: string;
    protocol_type?: Protocol.Type;

    // Agent 模式字段（link_id = 0 时返回）
    agent_id?: number;
    agent_endpoint_id?: number;

    // 资源权限
    can_edit?: boolean;
    can_delete?: boolean;
    can_share?: boolean;
    can_command?: boolean;
    access_level?: DeviceAccessLevel;
}

/** 设备选项（下拉列表） */
export interface DeviceOption {
    id: number;
    name: string;
    device_code: string;
    can_edit?: boolean;
    can_delete?: boolean;
    can_share?: boolean;
    can_command?: boolean;
    access_level?: DeviceAccessLevel;
}

/** 设备查询参数 */
export interface DeviceQuery extends PageParams {
    link_id?: number;
    protocol_config_id?: number;
    status?: DeviceStatus;
}

/** 创建设备 DTO */
export interface CreateDeviceDto {
    name: string;
    device_code: string;
    link_id: number;
    target_id?: string;
    protocol_config_id: number;
    group_id?: number | null;
    status?: DeviceStatus;
    /** 在线超时时间（秒），默认 300 秒（5分钟） */
    online_timeout?: number;
    /** 是否允许远控（下发指令），默认 true */
    remote_control?: boolean;
    /** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
    modbus_mode?: ModbusMode;
    /** Modbus 从站地址（1-247），默认 1 */
    slave_id?: number;
    /** 设备时区（默认 +08:00） */
    timezone?: string;
    /** 心跳包配置 */
    heartbeat?: HeartbeatConfig;
    /** 注册包配置 */
    registration?: RegistrationConfig;
    remark?: string;
    // Agent 模式字段（link_id = 0 时使用）
    agent_id?: number;
    agent_endpoint_id?: number;
}

/** 更新设备 DTO */
export interface UpdateDeviceDto {
    name?: string;
    device_code?: string;
    link_id?: number;
    target_id?: string;
    protocol_config_id?: number;
    group_id?: number | null;
    status?: DeviceStatus;
    /** 在线超时时间（秒） */
    online_timeout?: number;
    /** 是否允许远控（下发指令） */
    remote_control?: boolean;
    /** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
    modbus_mode?: ModbusMode;
    /** Modbus 从站地址（1-247），默认 1 */
    slave_id?: number;
    /** 设备时区 */
    timezone?: string;
    /** 心跳包配置 */
    heartbeat?: HeartbeatConfig;
    /** 注册包配置 */
    registration?: RegistrationConfig;
    remark?: string;
}

// ========== 设备静态数据类型（支持 ETag 缓存）==========

/** 设备静态数据（不包含实时数据，用于 ETag 缓存） */
export interface DeviceStaticData {
    // 基本信息
    id: number;
    name: string;
    /** 全局唯一设备编码 */
    device_code: string;
    link_id: number;
    target_id?: string;
    protocol_config_id: number;
    status: DeviceStatus;
    group_id?: number | null;
    online_timeout?: number;
    remote_control?: boolean;
    read_interval?: number;
    storage_interval?: number;
    /** 协议配置中的展示要素数量，用于稳定设备卡片布局 */
    element_count?: number;
    modbus_mode?: ModbusMode;
    slave_id?: number;
    timezone?: string;
    heartbeat?: HeartbeatConfig;
    registration?: RegistrationConfig;
    remark?: string;
    created_by?: number;
    created_at?: string;

    // 关联信息
    link_name?: string;
    link_mode?: Link.Mode;
    protocol_name?: string;
    protocol_type?: Protocol.Type;

    // 协议配置（按协议类型有条件返回）
    commandOperations?: CommandOperation[];
    imageOperations?: ImageOperation[];

    // Agent 模式字段（link_id = 0 时返回）
    agent_id?: number;
    agent_endpoint_id?: number;

    // 资源权限
    can_edit?: boolean;
    can_delete?: boolean;
    can_share?: boolean;
    can_command?: boolean;
    access_level?: DeviceAccessLevel;
}

/** 设备实时数据（用于轮询） */
export interface DeviceRealtimeData {
    id: number;
    reportTime?: string;
    /** 设备是否在线（基于实际 TCP 连接状态） */
    connected?: boolean;
    /** 设备连接状态（后端统一口径） */
    connectionState?: 'online' | 'offline';
    elements?: DeviceElement[];
    image?: { data: string };
    // 资源权限（realtime 接口同样返回）
    can_edit?: boolean;
    can_delete?: boolean;
    can_share?: boolean;
    can_command?: boolean;
    access_level?: DeviceAccessLevel;
}

// ========== 实时数据相关类型 ==========

/** 设备要素数据 */
export interface DeviceElement {
    name: string;
    value: string | number | null;
    unit?: string;
    /** 缩放系数（用于没有显式小数位时推导展示精度） */
    scale?: number;
    /** 小数位数（-1 或 undefined 表示原始值） */
    decimals?: number;
    /** 分组名称（由协议配置映射，用于设备管理页聚合展示） */
    group?: string;
    /** 编码类型 */
    encode?: string;
    /** 字典配置（用于映射值显示） */
    dictConfig?: {
        mapType: 'VALUE' | 'BIT';
        items: Array<{
            key: string;
            label: string;
            value?: string;
            dependsOn?: {
                operator: 'AND' | 'OR';
                conditions: Array<{
                    bitIndex: string;
                    bitValue: string;
                }>;
            };
        }>;
    };
}

/** 控制操作要素预设值 */
export interface CommandOperationElementOption {
    label: string;
    value: string;
}

/** 控制操作要素 */
export interface CommandOperationElement {
    elementId: string;
    name: string;
    value: string;
    unit?: string;
    /** 预设值选项（从协议配置继承） */
    options?: CommandOperationElementOption[];
    /** Modbus 寄存器类型 */
    registerType?: string;
    /** Modbus / S7 数据类型 */
    dataType?: string;
    /** S7 STRING 等类型的字节长度 */
    size?: number;
    /** SL651 编码类型 */
    encode?: string;
    /** SL651 数据长度（字节） */
    length?: number;
    /** SL651 BCD 小数位数 */
    digits?: number;
}

/** 控制操作 */
export interface CommandOperation {
    name: string;
    elements: CommandOperationElement[];
}

/** 图片操作要素 */
export interface ImageOperationElement {
    elementId: string;
    name: string;
    encode: string;
    unit?: string;
}

/** 图片操作 */
export interface ImageOperation {
    name: string;
    elements: ImageOperationElement[];
    /** 最新图片数据 */
    latestImage?: {
        data: string;
        size?: number;
    };
}

/** 设备实时数据（包含管理字段） = 静态数据 + 实时字段 */
export interface DeviceRealTimeData extends DeviceStaticData {
    /** 设备是否在线（基于实际 TCP 连接状态） */
    connected?: boolean;
    /** 设备连接状态（后端统一口径） */
    connectionState?: 'online' | 'offline';
    reportTime?: string;
    elements?: DeviceElement[];
    image?: { data: string };
}

/** 设备分享条目 */
export interface DeviceShareItem {
    target_type: 'user' | 'department';
    target_id: number;
    user_id?: number;
    username?: string;
    nickname?: string;
    department_id?: number;
    department_name?: string;
    permission: DeviceSharePermission;
    created_at?: string;
    updated_at?: string;
}

/** 设备分享更新参数 */
export interface DeviceShareUpsertDto {
    user_id?: number;
    user_ids?: number[];
    username?: string;
    department_ids?: number[];
    permission: DeviceSharePermission;
}

/** 指令下发参数 */
export interface CommandPayload {
    deviceCode: string;
    deviceId?: number;
    elements: Array<{ elementId: string; value: string }>;
}

// ========== 历史数据相关类型 ==========

/** 历史数据类型 */
export type HistoryDataType = 'ELEMENT' | 'IMAGE';

/** 历史设备列表项 */
export interface HistoryDeviceItem {
    code: string;
    name: string;
    typeName: string;
}

/** 指令状态 */
export type CommandStatus = 'PENDING' | 'SUCCESS' | 'TIMEOUT' | 'SEND_FAILED';

/** 要素历史记录 */
export interface ElementRecord {
    reportTime: string | Date;
    elements: DeviceElement[];
    /** 数据方向：UP=上行（设备上报），DOWN=下行（指令下发） */
    direction?: 'UP' | 'DOWN';
    /** 应答报文 ID（仅下行指令有，关联的应答记录） */
    responseId?: number;
    /** 应答报文解析的要素（仅下行指令有） */
    responseElements?: DeviceElement[];
    /** 发送用户 ID（仅下行指令有） */
    userId?: number;
    /** 发送用户名（仅下行指令有） */
    userName?: string;
    /** 指令状态（仅下行指令有） */
    status?: CommandStatus;
    /** 失败原因（仅下行指令失败时有） */
    failReason?: string;
}

/** 图片历史记录 */
export interface ImageRecord {
    reportTime: string | Date;
    size: number;
    data: string;
}

/** 历史数据查询参数 */
export interface HistoryDataQuery {
    page?: number;
    pageSize?: number;
    keyword?: string;
    /** 设备 ID */
    deviceId: number;
    /** 数据类型 */
    dataType: HistoryDataType;
    startTime?: Date;
    endTime?: Date;
}

type DeviceModbusMode = ModbusMode;
type DeviceHeartbeatConfig = HeartbeatConfig;
type DeviceRegistrationConfig = RegistrationConfig;
type DeviceCommandOperation = CommandOperation;
type DeviceCommandOperationElement = CommandOperationElement;
type DeviceImageOperation = ImageOperation;
type DeviceImageOperationElement = ImageOperationElement;
type DeviceAccessLevelType = DeviceAccessLevel;
type DeviceSharePermissionType = DeviceSharePermission;
type DeviceShareItemType = DeviceShareItem;
type DeviceShareUpsertDtoType = DeviceShareUpsertDto;
type DeviceCommandStatus = CommandStatus;

/** 设备模块命名空间 */
export namespace Device {
    export type ConnectionState = 'online' | 'offline';
    export type Status = DeviceStatus;
    export type ModbusMode = DeviceModbusMode;
    export type Item = DeviceItem;
    export type Option = DeviceOption;
    export type Query = DeviceQuery;
    export type CreateDto = CreateDeviceDto;
    export type UpdateDto = UpdateDeviceDto;
    export type HeartbeatConfig = DeviceHeartbeatConfig;
    export type RegistrationConfig = DeviceRegistrationConfig;

    // 静态数据（ETag 缓存）
    export type StaticData = DeviceStaticData;
    // 实时数据（轮询）
    export type Realtime = DeviceRealtimeData;

    // 合并后的完整数据
    export type Element = DeviceElement;
    export type RealTimeData = DeviceRealTimeData;
    export type Command = CommandPayload;
    export type CommandOperation = DeviceCommandOperation;
    export type CommandOperationElement = DeviceCommandOperationElement;
    export type ImageOperation = DeviceImageOperation;
    export type ImageOperationElement = DeviceImageOperationElement;
    export type AccessLevel = DeviceAccessLevelType;
    export type SharePermission = DeviceSharePermissionType;
    export type ShareItem = DeviceShareItemType;
    export type ShareUpsertDto = DeviceShareUpsertDtoType;

    // 历史数据
    export type HistoryDevice = HistoryDeviceItem;
    export type HistoryElement = ElementRecord;
    export type HistoryImage = ImageRecord;
    export type HistoryQuery = HistoryDataQuery;
    export type CommandStatus = DeviceCommandStatus;
}
