/**
 * 认证相关 Service
 */

import { useMutation, useQuery } from '@tanstack/react-query';
import { App } from 'antd';
import { useNavigate } from 'react-router-dom';
import { useAuthStore } from '@/store/authStore';
import type { Auth } from './login.types';
import { fetchCurrentUser, login, logout } from './login.api';

export const loginKeys = {
    currentUser: ['auth', 'currentUser'] as const,
};

export function useCurrentUser() {
    const token = useAuthStore((s) => s.token);
    const user = useAuthStore((s) => s.user);
    const setUser = useAuthStore((s) => s.setUser);

    return useQuery({
        queryKey: loginKeys.currentUser,
        queryFn: async () => {
            const freshUser = await fetchCurrentUser();
            setUser(freshUser);
            return freshUser;
        },
        enabled: !!token,
        initialData: user ?? undefined,
        initialDataUpdatedAt: 0,
        staleTime: 2 * 60 * 1000,
        refetchInterval: 5 * 60 * 1000,
        refetchOnWindowFocus: true,
    });
}

export function useLogin() {
    const navigate = useNavigate();
    const { message } = App.useApp();
    const setAuth = useAuthStore((s) => s.setAuth);

    return useMutation({
        mutationFn: login,
        onSuccess: (data: Auth.LoginResult) => {
            setAuth(data.token, data.refresh_token, data.user);
            message.success('登录成功');
            navigate('/', { replace: true });
        },
    });
}

export function useLogout() {
    const navigate = useNavigate();
    const clearAuth = useAuthStore((s) => s.clearAuth);

    return useMutation({
        mutationFn: () => logout(),
        onSettled: () => {
            clearAuth();
            navigate('/login', { replace: true });
        },
    });
}
