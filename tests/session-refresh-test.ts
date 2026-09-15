import { afterAll, afterEach, expect, test } from 'bun:test';
import request from '../web/lib/http';
import { refreshAccessToken } from '../web/pages/login/login.service';
import { useAuthStore } from '../web/store/authStore';

const storage = new Map<string, string>();
const previousStorage = Object.getOwnPropertyDescriptor(globalThis, 'sessionStorage');
Object.defineProperty(globalThis, 'sessionStorage', { configurable: true, value: {
    getItem: (key: string) => storage.get(key) ?? null,
    setItem: (key: string, value: string) => { storage.set(key, value); },
    removeItem: (key: string) => { storage.delete(key); },
}});
const originalAdapter = request.defaults.adapter;
const user = { id: 'user', username: 'tester', status: 'enabled', roles: [], permissions: [] };

afterEach(() => {
    useAuthStore.getState().clearAuth();
    request.defaults.adapter = originalAdapter;
});

test('concurrent refresh requests share one request and update the persisted session', async () => {
    useAuthStore.getState().setAuth('old-access', 'old-refresh', user);
    let calls = 0;
    let complete!: () => void;
    const gate = new Promise<void>(resolve => { complete = resolve; });
    request.defaults.adapter = async config => {
        calls++;
        await gate;
        return { config, status: 200, statusText: 'OK', headers: {}, data: { code: 0, message: 'ok', data: {
            token: 'new-access', refresh_token: 'new-refresh', user,
        } } };
    };
    const first = refreshAccessToken();
    const second = refreshAccessToken();
    expect(first).toBe(second);
    complete();
    expect(await first).toBeTrue();
    expect(calls).toBe(1);
    expect(useAuthStore.getState().token).toBe('new-access');
    expect(JSON.parse(storage.get('auth-storage')!).state.refresh_token).toBe('new-refresh');
});

test('failed refresh clears the session and no token means no request', async () => {
    let calls = 0;
    request.defaults.adapter = async () => { calls++; throw new Error('refresh failed'); };
    expect(await refreshAccessToken()).toBeFalse();
    expect(calls).toBe(0);
    useAuthStore.getState().setAuth('access', 'refresh', user);
    expect(await refreshAccessToken()).toBeFalse();
    expect(calls).toBe(1);
    expect(useAuthStore.getState().token).toBeNull();
    expect(useAuthStore.getState().user).toBeNull();
});

// Restore the host's storage descriptor after all tests in this module.
afterAll(() => {
    if (previousStorage) Object.defineProperty(globalThis, 'sessionStorage', previousStorage);
    else Reflect.deleteProperty(globalThis, 'sessionStorage');
});
