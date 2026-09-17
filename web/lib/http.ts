import { getMessageInstance } from '@/providers/MessageContextBridge';
import { useAuthStore } from '@/store/authStore';

export interface RequestConfig {
    params?: Record<string, unknown>;
    headers?: HeadersInit;
    signal?: AbortSignal;
    timeout?: number;
    keepalive?: boolean;
    onUploadProgress?: (loadedBytes: number, totalBytes: number) => void;
    _silent?: boolean;
}

export class HttpRequestError extends Error {
    constructor(
        message: string,
        readonly status: number,
        readonly code?: number
    ) {
        super(message);
        this.name = 'HttpRequestError';
    }
}

export function queryUrl(path: string, params?: Record<string, unknown>) {
    const query = new URLSearchParams();
    for (const [key, value] of Object.entries(params ?? {})) {
        if (value === undefined || value === null) continue;
        if (Array.isArray(value)) {
            for (const item of value) query.append(key, String(item));
        } else query.set(key, String(value));
    }
    const suffix = query.toString();
    return suffix ? `${path}${path.includes('?') ? '&' : '?'}${suffix}` : path;
}

export function uploadWithProgress(
    url: string,
    init: RequestInit,
    progress: (loadedBytes: number, totalBytes: number) => void,
    createRequest: () => XMLHttpRequest = () => new XMLHttpRequest()
): Promise<Response> {
    return new Promise((resolve, reject) => {
        const xhr = createRequest();
        const signal = init.signal;
        const cleanup = () => signal?.removeEventListener('abort', abort);
        const fail = (error: unknown) => {
            cleanup();
            reject(error);
        };
        const abort = () => {
            xhr.abort();
            fail(signal?.reason ?? new DOMException('请求已取消', 'AbortError'));
        };
        if (signal?.aborted) {
            abort();
            return;
        }
        xhr.open(init.method ?? 'POST', url);
        new Headers(init.headers).forEach((value, name) => {
            xhr.setRequestHeader(name, value);
        });
        xhr.upload.onprogress = (event) => {
            if (event.lengthComputable) progress(event.loaded, event.total);
        };
        xhr.onerror = () => fail(new TypeError('上传连接失败'));
        xhr.onabort = () => fail(signal?.reason ?? new DOMException('请求已取消', 'AbortError'));
        xhr.onload = () => {
            cleanup();
            if (xhr.status === 0) {
                reject(new TypeError('上传连接已断开'));
                return;
            }
            resolve(
                new Response(xhr.status === 204 || xhr.status === 304 ? null : xhr.responseText, {
                    status: xhr.status,
                    headers: {
                        'Content-Type': xhr.getResponseHeader('Content-Type') ?? 'application/json',
                    },
                })
            );
        };
        signal?.addEventListener('abort', abort, { once: true });
        try {
            xhr.send(init.body as XMLHttpRequestBodyInit | null | undefined);
        } catch (error) {
            fail(error);
        }
    });
}

export class HttpClient {
    constructor(
        private readonly options: {
            fetch: typeof fetch;
            token: () => string | null;
            refresh: () => Promise<boolean>;
            reportError?: (error: Error) => void;
        }
    ) {}

    get<T>(url: string, config?: RequestConfig) {
        return this.send<T>('GET', url, undefined, config);
    }
    post<T>(url: string, data?: unknown, config?: RequestConfig) {
        return this.send<T>('POST', url, data, config);
    }
    put<T>(url: string, data?: unknown, config?: RequestConfig) {
        return this.send<T>('PUT', url, data, config);
    }
    patch<T>(url: string, data?: unknown, config?: RequestConfig) {
        return this.send<T>('PATCH', url, data, config);
    }
    delete<T>(url: string, config?: RequestConfig & { data?: unknown }) {
        return this.send<T>('DELETE', url, config?.data, config);
    }

    private async send<T>(
        method: string,
        url: string,
        data?: unknown,
        config: RequestConfig = {}
    ): Promise<T> {
        const timeout = AbortSignal.timeout(config.timeout ?? 30000);
        const signal = config.signal ? AbortSignal.any([config.signal, timeout]) : timeout;
        const authRequest = url === '/v1/auth/login' || url === '/v1/auth/refresh';
        try {
            for (let attempt = 0; attempt < 2; attempt++) {
                signal.throwIfAborted();
                const headers = new Headers(config.headers);
                headers.set('Accept', 'application/json');
                const token = this.options.token();
                if (token && !authRequest) headers.set('Authorization', `Bearer ${token}`);
                let body: BodyInit | undefined;
                if (data instanceof FormData || data instanceof Blob) body = data;
                else if (data !== undefined) {
                    headers.set('Content-Type', 'application/json');
                    body = JSON.stringify(data);
                }
                const target = queryUrl(url, config.params);
                const init: RequestInit = {
                    method,
                    headers,
                    body,
                    signal,
                    keepalive: config.keepalive,
                };
                const response =
                    config.onUploadProgress && data instanceof Blob
                        ? await uploadWithProgress(target, init, config.onUploadProgress)
                        : await this.options.fetch(target, init);
                const text = await response.text();
                let envelope: { code?: number; message?: string; data?: T } | undefined;
                try {
                    envelope = text ? JSON.parse(text) : undefined;
                } catch {
                    throw new HttpRequestError('服务器返回了无效的 JSON', response.status);
                }
                if (
                    response.status === 401 &&
                    method === 'GET' &&
                    !authRequest &&
                    token &&
                    attempt === 0
                ) {
                    if (await this.options.refresh()) continue;
                }
                if (!response.ok || (envelope?.code !== undefined && envelope.code !== 0))
                    throw new HttpRequestError(
                        envelope?.message || `请求失败 (${response.status})`,
                        response.status,
                        envelope?.code
                    );
                return (envelope?.code !== undefined ? envelope.data : envelope) as T;
            }
            throw new HttpRequestError('登录已过期', 401);
        } catch (failure) {
            const error = failure instanceof Error ? failure : new Error(String(failure));
            if (!config._silent && !config.signal?.aborted) this.options.reportError?.(error);
            throw error;
        }
    }
}

let refresh: () => Promise<boolean> = async () => false;
export function configureSessionRefresh(callback: () => Promise<boolean>) {
    refresh = callback;
}
export function refreshSession() {
    return refresh();
}

const request = new HttpClient({
    fetch: (input, init) => fetch(input, init),
    token: () => useAuthStore.getState().token,
    refresh: refreshSession,
    reportError: (error) => getMessageInstance()?.error(error.message),
});
export default request;
