/**
 * 认证 API
 */

import type { Auth } from './login.types';
import request, { type RequestConfig } from '@/utils/http';

/** API 端点 */
const ENDPOINTS = {
    LOGIN: '/api/auth/login',
    REFRESH: '/api/auth/refresh',
    ME: '/api/auth/me',
    LOGOUT: '/api/auth/logout',
} as const;

/** 登录 */
export function login(params: Auth.LoginRequest) {
    return request.post<Auth.LoginResult>(ENDPOINTS.LOGIN, params);
}

/** 刷新 Token */
export function refreshToken(refreshToken: string, config?: RequestConfig) {
    return request.post<Auth.RefreshResult>(
        ENDPOINTS.REFRESH,
        {
            refresh_token: refreshToken,
        },
        config
    );
}

/** 获取当前用户信息 */
export function fetchCurrentUser(config?: RequestConfig) {
    return request.get<Auth.UserInfo>(ENDPOINTS.ME, config);
}

/** 登出 */
export function logout(config?: RequestConfig) {
    return request.post<void>(ENDPOINTS.LOGOUT, undefined, config);
}
