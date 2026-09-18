import type { PageParams } from '@/types/pagination';

type DeptStatus = 'enabled' | 'disabled';
interface DeptItem {
    id: string;
    name: string;
    code?: string;
    parent_id: string;
    parent_name?: string;
    leader_id: string;
    leader_name?: string;
    sort_order: number;
    status: DeptStatus;
}
interface DeptOption {
    id: string;
    name: string;
    parent_id: string;
}
interface DeptQuery extends PageParams {
    status?: DeptStatus;
    parent_id?: string;
}
interface CreateDeptDto {
    name: string;
    code?: string;
    parent_id?: string;
    leader_id?: string;
    sort_order?: number;
    status?: DeptStatus;
}
interface UpdateDeptDto extends Partial<CreateDeptDto> {}
export namespace Dept {
    export type Status = DeptStatus;
    export type Item = DeptItem;
    export type Option = DeptOption;
    export type Query = DeptQuery;
    export type CreateDto = CreateDeptDto;
    export type UpdateDto = UpdateDeptDto;
}
