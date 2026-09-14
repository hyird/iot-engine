import type { RequestConfig } from '@/utils/http';
import request from '@/utils/http';
import { createSnapshotStream } from '@/utils/snapshot-request';
import { loginSchema, refreshTokenSchema } from './login.schema';
import type { Auth } from './login.types';

/** API 端点 */
const ENDPOINTS = {
    LOGIN: '/v1/auth/login',
    REFRESH: '/v1/auth/refresh',
    ME: '/v1/auth/me',
    LOGOUT: '/v1/auth/logout',
} as const;
/** 登录 */
export function login(params: Auth.LoginRequest) {
    return request.post<Auth.LoginResult>(ENDPOINTS.LOGIN, loginSchema.parse(params));
}
/** 刷新 Token */
export function refreshToken(refreshToken: string, config?: RequestConfig) {
    const validatedToken = refreshTokenSchema.parse(refreshToken);
    return request.post<Auth.RefreshResult>(
        ENDPOINTS.REFRESH,
        {
            refresh_token: validatedToken,
        },
        config
    );
}
/** 获取当前用户信息 */
export function fetchCurrentUser(config?: RequestConfig) {
    return createSnapshotStream<Auth.UserInfo>(ENDPOINTS.ME, config);
}
/** 登出 */
export function logout(config?: RequestConfig) {
    return request.post<void>(ENDPOINTS.LOGOUT, undefined, config);
}
