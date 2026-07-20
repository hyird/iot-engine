import { useAuthStore } from '@/store/authStore';

export function usePermissions() {
    const permissions = useAuthStore((state) => state.user?.permissions ?? []);
    const has = (permission: string) =>
        permissions.includes('*') || permissions.includes(permission);
    return { has, permissions };
}
