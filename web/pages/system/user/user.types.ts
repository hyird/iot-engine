/**
 * 用户管理类型定义
 */

import type { PageParams } from '@/utils/types';
import { createQueryKeys } from '@/utils/query';

// ============ QueryKeys ============

export const userKeys = createQueryKeys('users');

export const userQueryKeys = {
    ...userKeys,
    list: (params?: User.Query) => [...userKeys.lists(), params] as const,
};

export const roleOptionQueryKey = ['roles', 'options'] as const;

// ============ 枚举/状态类型 ============

export type UserStatus = 'enabled' | 'disabled';

// ============ 基础类型 ============

export interface UserRole {
    id: number;
    name: string;
    code: string;
}

export interface UserOption {
    id: number;
    username: string;
    nickname?: string;
    phone?: string;
    email?: string;
}

// ============ 列表项/详情类型 ============

export interface UserItem {
    id: number;
    username: string;
    nickname?: string;
    phone?: string;
    email?: string;
    status: UserStatus;
    roles: UserRole[];
    created_at?: string;
    updated_at?: string;
}

// ============ 查询参数 ============

export interface UserQuery extends PageParams {
    status?: UserStatus;
}

// ============ DTO 类型 ============

export interface CreateUserDto {
    username: string;
    password: string;
    nickname?: string;
    phone?: string;
    email?: string;
    status?: UserStatus;
    role_ids?: number[];
}

export interface UpdateUserDto {
    nickname?: string;
    phone?: string;
    email?: string;
    status?: UserStatus;
    password?: string;
    role_ids?: number[];
}

export interface UserUpdatePasswordDto {
    old_password: string;
    new_password: string;
}

export namespace User {
    export type Status = UserStatus;
    export type Role = UserRole;
    export type Option = UserOption;
    export type Item = UserItem;
    export type Query = UserQuery;
    export type CreateDto = CreateUserDto;
    export type UpdateDto = UpdateUserDto;
    export type UpdatePasswordDto = UserUpdatePasswordDto;
}
