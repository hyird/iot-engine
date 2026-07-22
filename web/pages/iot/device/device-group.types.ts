/**
 * 设备分组类型定义
 */

export type DeviceGroupStatus = 'enabled' | 'disabled';

/** 设备分组列表项 */
export interface DeviceGroupItem {
    id: string;
    name: string;
    parent_id?: string | null;
    status: DeviceGroupStatus;
    sort_order: number;
    remark?: string;
    created_at?: string;
    updated_at?: string;
    can_share?: boolean;
}

/** 设备分组树节点 */
export interface DeviceGroupTreeItem extends DeviceGroupItem {
    children?: DeviceGroupTreeItem[];
    /** 分组下的设备数量（treeWithCount 接口返回） */
    deviceCount?: number;
}

/** 创建设备分组 DTO */
export interface CreateDeviceGroupDto {
    name: string;
    parent_id?: string | null;
    status?: DeviceGroupStatus;
    sort_order?: number;
    remark?: string;
}

/** 更新设备分组 DTO */
export interface UpdateDeviceGroupDto {
    name?: string;
    parent_id?: string | null;
    status?: DeviceGroupStatus;
    sort_order?: number;
    remark?: string;
}

/** 设备分组命名空间 */
export declare namespace DeviceGroup {
    type Status = DeviceGroupStatus;
    type Item = DeviceGroupItem;
    type TreeItem = DeviceGroupTreeItem;
    type CreateDto = CreateDeviceGroupDto;
    type UpdateDto = UpdateDeviceGroupDto;
}
