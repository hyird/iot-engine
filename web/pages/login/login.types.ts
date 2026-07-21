/**
 * 登录相关类型定义
 */

export namespace Auth {
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

    export interface UserInfo {
        id: string;
        username: string;
        nickname?: string;
        status: string;
        roles: RoleOption[];
        permissions: string[];
    }

    interface RoleOption {
        id: string;
        name: string;
        code: string;
    }
}
