import { useCallback, useMemo } from 'react';
import { useAuthStore } from '@/store/authStore';

const EMPTY_PERMISSIONS: string[] = [];

export function usePermissions() {
    const permissions = useAuthStore((state) => state.user?.permissions ?? EMPTY_PERMISSIONS);
    const has = useCallback(
        (permission: string) => permissions.includes('*') || permissions.includes(permission),
        [permissions]
    );
    return useMemo(() => ({ has, permissions }), [has, permissions]);
}

export function usePermission(permission: string) {
    const { has } = usePermissions();
    return has(permission);
}
