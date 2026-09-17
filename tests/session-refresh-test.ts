import { afterAll, afterEach, expect, test, spyOn } from 'bun:test';
import request, { HttpRequestError } from '../web/lib/http';
import { refreshAccessToken } from '../web/pages/login/login.service';
import { useAuthStore } from '../web/store/authStore';

const storage = new Map<string, string>();
const previousStorage = Object.getOwnPropertyDescriptor(globalThis, 'sessionStorage');
Object.defineProperty(globalThis, 'sessionStorage', { configurable: true, value: {
    getItem: (key: string) => storage.get(key) ?? null,
    setItem: (key: string, value: string) => { storage.set(key, value); },
    removeItem: (key: string) => { storage.delete(key); },
}});
const send = spyOn(request, 'post');
const user = { id: 'user', username: 'tester', status: 'enabled', roles: [], permissions: [] };

afterEach(() => {
    useAuthStore.getState().clearAuth();
    send.mockReset();
});

test('concurrent refresh requests share one request and update the persisted session', async () => {
    useAuthStore.getState().setAuth('old-access', 'old-refresh', user);
    let calls = 0;
    let complete!: () => void;
    const gate = new Promise<void>(resolve => { complete = resolve; });
    send.mockImplementation(async (event, data) => {
        expect(event).toBe('/v1/auth/refresh');
        expect(data).toEqual({ refresh_token: 'old-refresh' });
        calls++;
        await gate;
        return {
            token: 'new-access', refresh_token: 'new-refresh', user,
        };
    });
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
    send.mockImplementation(async () => { calls++; throw new HttpRequestError('refresh failed', 401, 11006); });
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
    send.mockRestore();
    if (previousStorage) Object.defineProperty(globalThis, 'sessionStorage', previousStorage);
    else Reflect.deleteProperty(globalThis, 'sessionStorage');
});

test('network failure preserves credentials for reconnect', async () => {
    useAuthStore.getState().setAuth('access', 'refresh', user);
    send.mockImplementation(async () => { throw new Error('network failed'); });
    expect(await refreshAccessToken()).toBeFalse();
    expect(useAuthStore.getState().token).toBe('access');
});

test('a late refresh cannot overwrite a replacement account', async () => {
    useAuthStore.getState().setAuth('old-access', 'old-refresh', user);
    let complete!: () => void;
    const gate = new Promise<void>(resolve => { complete = resolve; });
    send.mockImplementation(async () => {
        await gate;
        return { token: 'stale-access', refresh_token: 'stale-refresh', user };
    });
    const pending = refreshAccessToken();
    useAuthStore.getState().setAuth('other-access', 'other-refresh', { ...user, id: 'other' });
    complete();
    expect(await pending).toBeFalse();
    expect(useAuthStore.getState().token).toBe('other-access');
    expect(useAuthStore.getState().user?.id).toBe('other');
});
