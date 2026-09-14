import { useMutation } from '@tanstack/react-query';
import { useNavigate } from 'react-router-dom';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import { useAuthStore } from '@/store/authStore';
import { fetchCurrentUser, logout, refreshToken } from './login.api';

const loginKeys = {
    currentUser: ['auth', 'currentUser'] as const,
};
export function useCurrentUser() {
    const token = useAuthStore((s) => s.token);
    const user = useAuthStore((s) => s.user);
    const setUser = useAuthStore((s) => s.setUser);
    return useSnapshotQuery({
        queryKey: loginKeys.currentUser,
        queryFn: () =>
            fetchCurrentUser().map((freshUser) => {
                setUser(freshUser);
                return freshUser;
            }),
        enabled: !!token,
        initialData: user ?? undefined,
        initialDataUpdatedAt: 0,
        staleTime: 2 * 60 * 1000,
        refetchOnWindowFocus: true,
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

let pendingRefresh: Promise<boolean> | undefined;
export function refreshAccessToken(): Promise<boolean> {
    if (pendingRefresh) return pendingRefresh;
    const currentRefreshToken = useAuthStore.getState().refresh_token;
    if (!currentRefreshToken) return Promise.resolve(false);
    pendingRefresh = refreshToken(currentRefreshToken, { _silent: true })
        .then(({ token, refresh_token, user }) => {
            useAuthStore.getState().setAuth(token, refresh_token, user);
            return true;
        })
        .catch(() => {
            useAuthStore.getState().clearAuth();
            return false;
        })
        .finally(() => {
            pendingRefresh = undefined;
        });
    return pendingRefresh;
}

export { login } from './login.api';
