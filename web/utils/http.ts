/**
 * HTTP 请求基础配置
 */

import { notification } from 'antd';
import axios, {
    type AxiosError,
    type AxiosInstance,
    type AxiosRequestConfig,
    type AxiosResponse,
    type InternalAxiosRequestConfig,
} from 'axios';
import { getMessage } from '@/providers/Message';
import { useAuthStore } from '@/store/authStore';

function normalizePath(path: string) {
    return path.startsWith('/') ? path : `/${path}`;
}

function getHashRoutePath(path: string) {
    const baseUrl = (import.meta.env.BASE_URL || '/').replace(/\/$/, '');
    return `${baseUrl || ''}/#${normalizePath(path)}`;
}

function redirectToLogin() {
    if (window.location.hash === '#/login') {
        return;
    }
    window.location.replace(getHashRoutePath('/login'));
}

type ApiErrorSource = 'response' | 'network' | 'timeout' | 'server' | 'auth-refresh';

interface ApiResponseEnvelope<T = unknown> {
    code: number;
    message: string;
    data?: T;
    status?: number;
}

interface ApiErrorOptions {
    code?: number;
    status?: number;
    data?: unknown;
    source?: ApiErrorSource;
}

class ApiError extends Error {
    code?: number;
    status?: number;
    data?: unknown;
    source: ApiErrorSource;

    constructor(message: string, options: ApiErrorOptions = {}) {
        super(message);
        this.name = 'ApiError';
        this.code = options.code;
        this.status = options.status;
        this.data = options.data;
        this.source = options.source ?? 'response';

        Object.setPrototypeOf(this, ApiError.prototype);
    }
}

function isApiResponseEnvelope(value: unknown): value is ApiResponseEnvelope {
    if (typeof value !== 'object' || value === null) return false;

    const record = value as Record<string, unknown>;
    return typeof record.code === 'number' && typeof record.message === 'string';
}

function getApiResponseMessage(value: unknown, fallback = '请求失败'): string {
    if (!isApiResponseEnvelope(value)) return fallback;

    const message = value.message.trim();
    return message || fallback;
}

function getApiResponseCode(value: unknown): number | undefined {
    return isApiResponseEnvelope(value) ? value.code : undefined;
}

export interface RequestConfig extends AxiosRequestConfig {
    _retry?: boolean;
    _silent?: boolean;
}

interface RequestInstance extends AxiosInstance {
    get<T = unknown>(url: string, config?: RequestConfig): Promise<T>;
    post<T = unknown>(url: string, data?: unknown, config?: RequestConfig): Promise<T>;
    put<T = unknown>(url: string, data?: unknown, config?: RequestConfig): Promise<T>;
    delete<T = unknown>(url: string, config?: RequestConfig): Promise<T>;
    patch<T = unknown>(url: string, data?: unknown, config?: RequestConfig): Promise<T>;
}

const request = axios.create({
    baseURL: '/',
    timeout: 30000,
}) as RequestInstance;

function buildApiError(
    message: string,
    options: {
        code?: number;
        status?: number;
        data?: unknown;
        source?: ApiError['source'];
    } = {}
) {
    return new ApiError(message, options);
}

function buildApiErrorFromResponse(
    response: { data: unknown; status: number },
    fallbackMessage = '请求失败'
) {
    const payload = response.data;
    const message = getApiResponseMessage(payload, fallbackMessage);
    const code = getApiResponseCode(payload);

    return buildApiError(message, {
        code,
        status: response.status,
        data: payload,
        source: 'response',
    });
}

function buildApiErrorFromAxiosError(error: AxiosError<unknown>) {
    if (!error.response) {
        const isTimeout = error.code === 'ECONNABORTED';
        return buildApiError(isTimeout ? '请求超时' : '网络连接失败', {
            status: isTimeout ? 408 : 0,
            data: error,
            source: isTimeout ? 'timeout' : 'network',
        });
    }

    return buildApiErrorFromResponse(
        {
            data: error.response.data,
            status: error.response.status,
        },
        error.message || '请求失败'
    );
}

function notifyTransportIssue(error: AxiosError<unknown>) {
    if (error.code === 'ECONNABORTED') {
        notification.error({
            message: '请求超时',
            description: '网络连接较慢，请稍后重试',
        });
        return;
    }

    notification.error({
        message: '网络连接失败',
        description: '请检查网络连接是否正常',
    });
}

function createExpiredAuthError() {
    return buildApiError('登录状态已失效，请重新登录', {
        status: 401,
        source: 'auth-refresh',
    });
}

function hasAuthorizationHeader(
    headers?: InternalAxiosRequestConfig['headers'] | RequestConfig['headers']
) {
    if (!headers) return false;

    const normalizedHeaders = headers as Record<string, unknown> & {
        has?: (name: string) => boolean;
    };

    return typeof normalizedHeaders.has === 'function'
        ? normalizedHeaders.has('Authorization') || normalizedHeaders.has('authorization')
        : 'Authorization' in normalizedHeaders || 'authorization' in normalizedHeaders;
}

request.interceptors.request.use(
    (config: InternalAxiosRequestConfig) => {
        const token = useAuthStore.getState().token;
        if (token && config.headers) {
            if (!hasAuthorizationHeader(config.headers)) {
                const headers = config.headers as Record<string, unknown>;
                headers.Authorization = `Bearer ${token}`;
            }
        }
        return config;
    },
    (error) => Promise.reject(error)
);

let isRefreshing = false;
let refreshSubscribers: Array<{
    onSuccess: (token: string) => void;
    onError: (error: ApiError) => void;
}> = [];

function subscribeTokenRefresh(
    onSuccess: (token: string) => void,
    onError: (error: ApiError) => void
) {
    refreshSubscribers.push({ onSuccess, onError });
}

function onTokenRefreshed(token: string) {
    const subscribers = refreshSubscribers;
    refreshSubscribers = [];

    subscribers.forEach(({ onSuccess }) => {
        onSuccess(token);
    });
}

function onTokenRefreshFailed(error: ApiError) {
    const subscribers = refreshSubscribers;
    refreshSubscribers = [];

    subscribers.forEach(({ onError }) => {
        onError(error);
    });
}

function handleAuthExpired(error: ApiError): never {
    isRefreshing = false;
    onTokenRefreshFailed(error);
    useAuthStore.getState().clearAuth();
    redirectToLogin();
    throw error;
}

request.interceptors.response.use(
    (response): AxiosResponse => {
        const data = response.data as unknown;
        const isSilent = (response.config as RequestConfig | undefined)?._silent;
        const responseCode = isApiResponseEnvelope(data) ? data.code : undefined;
        const isSuccessCode = responseCode === 0;

        if (isApiResponseEnvelope(data) && !isSuccessCode) {
            const apiError = buildApiErrorFromResponse({
                data,
                status: response.status,
            });
            if (!isSilent) {
                getMessage()?.error(apiError.message);
            }
            throw apiError;
        }

        return (
            isApiResponseEnvelope(data) && data.data !== undefined ? data.data : data
        ) as AxiosResponse;
    },
    async (error: AxiosError<unknown>): Promise<AxiosResponse> => {
        const originalRequest = error.config as RequestConfig | undefined;
        const requestUrl = originalRequest?.url || '';
        const isSilent = originalRequest?._silent ?? false;
        const isAuthRefreshRequest = requestUrl.includes('/api/auth/refresh');
        const hasAuthHeader = hasAuthorizationHeader(originalRequest?.headers);

        if (isSilent) {
            throw buildApiErrorFromAxiosError(error);
        }

        if (
            error.response?.status === 401 &&
            originalRequest &&
            !originalRequest._retry &&
            hasAuthHeader &&
            !isAuthRefreshRequest
        ) {
            if (isRefreshing) {
                return new Promise<AxiosResponse>((resolve, reject) => {
                    subscribeTokenRefresh(
                        (token: string) => {
                            if (originalRequest.headers) {
                                originalRequest.headers.Authorization = `Bearer ${token}`;
                            }
                            resolve(request(originalRequest));
                        },
                        (refreshError: ApiError) => {
                            reject(refreshError);
                        }
                    );
                });
            }

            originalRequest._retry = true;
            isRefreshing = true;

            try {
                const success = await useAuthStore.getState().refreshAccessToken();
                if (success) {
                    const newToken = useAuthStore.getState().token;
                    // biome-ignore lint/style/noNonNullAssertion: refreshAccessToken returns true only when token is set
                    onTokenRefreshed(newToken!);
                    if (originalRequest.headers) {
                        originalRequest.headers.Authorization = `Bearer ${newToken}`;
                    }
                    return request(originalRequest);
                }

                return handleAuthExpired(createExpiredAuthError());
            } catch {
                return handleAuthExpired(createExpiredAuthError());
            } finally {
                isRefreshing = false;
            }
        }

        const apiError = buildApiErrorFromAxiosError(error);

        if (!error.response) {
            notifyTransportIssue(error);
            throw apiError;
        }

        if (error.response.status >= 500) {
            notification.error({
                message: '服务器错误',
                description: '服务器遇到问题，请稍后重试',
            });
            throw buildApiError(apiError.message || '服务器错误', {
                code: apiError.code,
                status: apiError.status ?? error.response.status,
                data: apiError.data,
                source: 'server',
            });
        }

        getMessage()?.error(apiError.message || error.message || '请求失败');
        throw apiError;
    }
);

export default request;
