import { Spin } from 'antd';
import { lazy, Suspense } from 'react';
import { createHashRouter, Navigate, Outlet, RouterProvider, useLocation } from 'react-router-dom';
import { useAuthStore } from '@/store/authStore';

const LoginPage = lazy(() => import('@/pages/login'));
const AdminLayout = lazy(() => import('@/layouts/AdminLayout'));
const RolePage = lazy(() => import('@/pages/system/role'));
const DeptPage = lazy(() => import('@/pages/system/dept'));
const UserPage = lazy(() => import('@/pages/system/user'));
const LinkPage = lazy(() => import('@/pages/iot/link'));

function AuthGuard() {
    const token = useAuthStore((state) => state.token);
    const location = useLocation();
    if (!token) {
        return <Navigate to="/login" replace state={{ from: { pathname: location.pathname } }} />;
    }
    return <Outlet />;
}

const router = createHashRouter([
    {
        path: '/login',
        element: <LoginPage />,
    },
    {
        element: <AuthGuard />,
        children: [
            {
                path: '/',
                element: <AdminLayout />,
                children: [
                    { index: true, element: <Navigate to="/system/role" replace /> },
                    { path: 'system/role', element: <RolePage /> },
                    { path: 'system/dept', element: <DeptPage /> },
                    { path: 'system/user', element: <UserPage /> },
                    { path: 'iot/link', element: <LinkPage /> },
                ],
            },
        ],
    },
    { path: '*', element: <Navigate to="/" replace /> },
]);

export function AppRoutes() {
    return (
        <Suspense
            fallback={
                <div className="flex h-screen items-center justify-center">
                    <Spin size="large" />
                </div>
            }
        >
            <RouterProvider router={router} />
        </Suspense>
    );
}
