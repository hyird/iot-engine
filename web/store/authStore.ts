import { create, type StateCreator } from 'zustand';
import { persist } from 'zustand/middleware';
import type { Auth } from '@/pages/login/login.types';
import { refreshToken } from '@/pages/login/login.api';

interface AuthState {
    token: string | null;
    refresh_token: string | null;
    user: Auth.UserInfo | null;
}

interface AuthActions {
    setAuth: (token: string, refresh_token: string, user: Auth.UserInfo) => void;
    clearAuth: () => void;
    setUser: (user: Auth.UserInfo) => void;
    refreshAccessToken: () => Promise<boolean>;
}

export type AuthStore = AuthState & AuthActions;

const authStateCreator: StateCreator<AuthStore> = (set, get) => ({
    token: null,
    refresh_token: null,
    user: null,

    setAuth: (token, refresh_token, user) => {
        set({ token, refresh_token, user });
    },

    clearAuth: () => {
        set({ token: null, refresh_token: null, user: null });
    },

    setUser: (user) => {
        set({ user });
    },

    refreshAccessToken: async () => {
        const { refresh_token: currentRefreshToken } = get();
        if (!currentRefreshToken) return false;

        try {
            const {
                token,
                refresh_token: nextRefreshToken,
                user,
            } = await refreshToken(currentRefreshToken, {
                _silent: true,
            });
            set({ token, refresh_token: nextRefreshToken, user });
            return true;
        } catch {
            set({ token: null, refresh_token: null, user: null });
            return false;
        }
    },
});

export const useAuthStore = create<AuthStore>()(
    persist(authStateCreator, {
        name: 'auth-storage',
        storage: {
            getItem: (key) => {
                const value = sessionStorage.getItem(key);
                return value ? JSON.parse(value) : null;
            },
            setItem: (key, value) => {
                sessionStorage.setItem(key, JSON.stringify(value));
            },
            removeItem: (key) => {
                sessionStorage.removeItem(key);
            },
        },
        partialize: (state) =>
            ({
                token: state.token,
                refresh_token: state.refresh_token,
                user: state.user,
            }) as AuthStore,
    })
);
