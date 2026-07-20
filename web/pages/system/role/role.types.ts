import type { PageParams } from '@/utils/types';
import { createQueryKeys } from '@/utils/query';

export const roleKeys = createQueryKeys('roles');
export const roleQueryKeys = {
    ...roleKeys,
    list: (params?: Role.Query) => [...roleKeys.lists(), params] as const,
};

export type RoleStatus = 'enabled' | 'disabled';

export interface RoleItem {
    id: number;
    name: string;
    code: string;
    description?: string;
    status: RoleStatus;
    permissions: string[];
    created_at?: string;
    updated_at?: string;
}

export interface RoleQuery extends PageParams {
    status?: RoleStatus;
}

export interface CreateRoleDto {
    name: string;
    code: string;
    description?: string;
    status?: RoleStatus;
    permissions?: string[];
}

export interface UpdateRoleDto extends Partial<CreateRoleDto> {}

export namespace Role {
    export type Status = RoleStatus;
    export type Item = RoleItem;
    export type Query = RoleQuery;
    export type CreateDto = CreateRoleDto;
    export type UpdateDto = UpdateRoleDto;
}
