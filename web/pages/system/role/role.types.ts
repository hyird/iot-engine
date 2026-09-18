import type { PageParams } from '@/types/pagination';

type RoleStatus = 'enabled' | 'disabled';
interface RoleItem {
    id: string;
    name: string;
    code: string;
    description?: string;
    status: RoleStatus;
    permissions: string[];
    created_at?: string;
    updated_at?: string;
}
interface RoleQuery extends PageParams {
    status?: RoleStatus;
}
interface CreateRoleDto {
    name: string;
    code: string;
    description?: string;
    status?: RoleStatus;
    permissions?: string[];
}
interface UpdateRoleDto extends Partial<CreateRoleDto> {}
export namespace Role {
    export type Option = Pick<RoleItem, 'id' | 'name' | 'code'>;
    export type Status = RoleStatus;
    export type Item = RoleItem;
    export type Query = RoleQuery;
    export type CreateDto = CreateRoleDto;
    export type UpdateDto = UpdateRoleDto;
}
