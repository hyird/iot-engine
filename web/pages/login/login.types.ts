/**
 * 登录相关类型定义
 */

// ============ JWT 相关 ============

export interface JwtPayload {
    user_id: number;
    username: string;
    iat?: number;
    exp?: number;
}

// ============ 登录相关 ============

export interface LoginRequest {
    username: string;
    password: string;
}

export interface LoginResult {
    token: string;
    refresh_token: string;
    user: UserInfo;
}

export interface RefreshResult {
    token: string;
    refresh_token: string;
    user: UserInfo;
}

// ============ 用户信息 ============

export interface UserInfo {
    id: number;
    username: string;
    nickname?: string;
    status: string;
    roles: RoleOption[];
    permissions: string[];
}

export interface RoleOption {
    id: number;
    name: string;
    code: string;
}

export namespace Auth {
    export type JwtPayload = import('./login.types').JwtPayload;
    export type LoginRequest = import('./login.types').LoginRequest;
    export type LoginResult = import('./login.types').LoginResult;
    export type RefreshResult = import('./login.types').RefreshResult;
    export type UserInfo = import('./login.types').UserInfo;
}
