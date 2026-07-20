import type { PageParams } from '@/utils/types';
import { createQueryKeys } from '@/utils/query';

export const deptKeys = createQueryKeys('departments');
export const deptQueryKeys = {
    ...deptKeys,
    list: (params?: Dept.Query) => [...deptKeys.lists(), params] as const,
};
export type DeptStatus = 'enabled' | 'disabled';

export interface DeptItem {
    id: number;
    name: string;
    code?: string;
    parent_id: number;
    parent_name?: string;
    leader_id: number;
    leader_name?: string;
    sort_order: number;
    status: DeptStatus;
}
export interface DeptOption {
    id: number;
    name: string;
    parent_id: number;
}
export interface DeptQuery extends PageParams {
    status?: DeptStatus;
    parent_id?: number;
}
export interface CreateDeptDto {
    name: string;
    code?: string;
    parent_id?: number;
    leader_id?: number;
    sort_order?: number;
    status?: DeptStatus;
}
export interface UpdateDeptDto extends Partial<CreateDeptDto> {}
export namespace Dept {
    export type Status = DeptStatus;
    export type Item = DeptItem;
    export type Option = DeptOption;
    export type Query = DeptQuery;
    export type CreateDto = CreateDeptDto;
    export type UpdateDto = UpdateDeptDto;
}
