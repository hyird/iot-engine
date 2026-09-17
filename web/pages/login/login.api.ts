import request, { type RequestConfig } from '@/lib/http';
import { createSharedSseSnapshotStream } from '@/lib/snapshot-request';
import { loginSchema, refreshTokenSchema } from './login.schema';
import type { Auth } from './login.types';

/** 登录 */
export function login(params: Auth.LoginRequest) {
    return request.post<Auth.LoginResult>('/v1/auth/login', loginSchema.parse(params));
}
/** 刷新 Token */
export function refreshToken(refreshToken: string, config?: RequestConfig) {
    const validatedToken = refreshTokenSchema.parse(refreshToken);
    return request.post<Auth.RefreshResult>(
        '/v1/auth/refresh',
        {
            refresh_token: validatedToken,
        },
        config
    );
}
/** 获取当前用户信息 */
export function fetchCurrentUser(signal?: AbortSignal) {
    return request.get<Auth.UserInfo>('/v1/auth/me', { signal });
}
/** 仅观察已有实时连接，不建立独立订阅。 */
export function observeCurrentUser() {
    return createSharedSseSnapshotStream<Auth.UserInfo>('user');
}
/** 登出 */
export function logout(config?: RequestConfig) {
    return request.post<void>('/v1/auth/logout', undefined, config);
}
