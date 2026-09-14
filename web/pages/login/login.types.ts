import type { SessionUser } from '@/types/session';

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
    export type UserInfo = SessionUser;
}
