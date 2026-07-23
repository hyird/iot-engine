import { Button, Result, Spin } from 'antd';
import { lazy, Suspense } from 'react';
import { createHashRouter, Navigate, Outlet, RouterProvider, useLocation } from 'react-router-dom';
import { useAuthStore } from '@/store/authStore';

const LoginPage = lazy(() => import('@/pages/login'));
const AdminLayout = lazy(() => import('@/layouts/AdminLayout'));
const RolePage = lazy(() => import('@/pages/system/role'));
const DeptPage = lazy(() => import('@/pages/system/dept'));
const UserPage = lazy(() => import('@/pages/system/user'));
const LinkPage = lazy(() => import('@/pages/iot/link'));
const ModbusConfigPage = lazy(() => import('@/pages/iot/protocol/ModbusConfig'));
const SL651ConfigPage = lazy(() => import('@/pages/iot/protocol/SL651Config'));
const S7ConfigPage = lazy(() => import('@/pages/iot/protocol/S7Config'));
const DevicePage = lazy(() => import('@/pages/iot/device'));
const AccessPage = lazy(() => import('@/pages/iot/open-access'));
const EdgeNodePage = lazy(() => import('@/pages/iot/edge-node'));

const routeErrorElement = (
    <div className="flex h-screen items-center justify-center p-6">
        <Result
            status="error"
            title="页面加载失败"
            subTitle="页面运行时出现异常，请刷新后重试。"
            extra={
                <Button type="primary" onClick={() => window.location.reload()}>
                    刷新页面
                </Button>
            }
        />
    </div>
);

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
        errorElement: routeErrorElement,
    },
    {
        element: <AuthGuard />,
        errorElement: routeErrorElement,
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
                    { path: 'iot/modbus', element: <ModbusConfigPage /> },
                    { path: 'iot/sl651', element: <SL651ConfigPage /> },
                    { path: 'iot/s7', element: <S7ConfigPage /> },
                    { path: 'device', element: <DevicePage /> },
                    { path: 'iot/open-access', element: <AccessPage /> },
                    { path: 'iot/edge', element: <EdgeNodePage /> },
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
