/**
 * 认证
 */

export { useCurrentUser, useLogout } from './login.service';

import {
    ClusterOutlined,
    HddOutlined,
    LockOutlined,
    SafetyCertificateOutlined,
    UserOutlined,
} from '@ant-design/icons';
import { useMutation } from '@tanstack/react-query';
import { Button, Form, Input, theme } from 'antd';
import { useEffect } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { APP_NAME, getAppTitle } from '@/config/app';
import { useAuthStore } from '@/store/authStore';
import { validateForm } from '@/utils/validation';
import { login } from './login.client';
import { loginSchema } from './login.schema';
import type { Auth } from './login.types';

interface LocationState {
    from?: {
        pathname: string;
    };
}

const pageTitle = getAppTitle('登录系统');

function LoginPage() {
    const [form] = Form.useForm<Auth.LoginRequest>();
    const navigate = useNavigate();
    const location = useLocation();
    const { token, setAuth } = useAuthStore();
    const { token: themeToken } = theme.useToken();

    const mutation = useMutation({
        mutationFn: login,
        onSuccess: (data) => {
            setAuth(data.token, data.refresh_token, data.user);
        },
    });

    useEffect(() => {
        if (token && !mutation.isPending) {
            const from = (location.state as LocationState)?.from?.pathname || '/home';
            navigate(from, { replace: true });
        }
    }, [token, mutation.isPending, location.state, navigate]);

    useEffect(() => {
        const previousTitle = document.title;
        document.title = pageTitle;

        return () => {
            document.title = previousTitle;
        };
    }, []);

    const onFinish = (values: Auth.LoginRequest) => {
        if (mutation.isPending) return;
        const validated = validateForm(form, loginSchema, values);
        if (validated) mutation.mutate(validated);
    };

    return (
        <main
            className="flex min-h-screen items-center justify-center overflow-hidden p-4 sm:p-8"
            style={{ background: themeToken.colorBgLayout }}
        >
            <div
                className="grid w-full max-w-[1120px] overflow-hidden rounded-xl lg:grid-cols-[1.08fr_0.92fr]"
                style={{
                    background: themeToken.colorBgContainer,
                    border: `1px solid ${themeToken.colorBorderSecondary}`,
                    boxShadow: themeToken.boxShadowSecondary,
                }}
            >
                <section
                    className="relative hidden min-h-[620px] flex-col overflow-hidden p-12 lg:flex"
                    style={{
                        color: themeToken.colorText,
                        background: `linear-gradient(145deg, ${themeToken.colorPrimaryBg} 0%, ${themeToken.colorPrimaryBgHover} 58%, ${themeToken.colorInfoBg} 100%)`,
                    }}
                >
                    <div
                        className="absolute inset-0"
                        style={{
                            backgroundImage: `linear-gradient(${themeToken.colorBorderSecondary} 1px, transparent 1px), linear-gradient(90deg, ${themeToken.colorBorderSecondary} 1px, transparent 1px)`,
                            backgroundSize: '40px 40px',
                            maskImage: 'linear-gradient(to bottom right, black, transparent 78%)',
                        }}
                        aria-hidden="true"
                    />
                    <div
                        className="absolute -right-20 top-28 size-72 rounded-full border"
                        style={{ borderColor: themeToken.colorPrimaryBorder }}
                    />
                    <div
                        className="absolute -right-6 top-44 size-44 rounded-full border"
                        style={{ borderColor: themeToken.colorPrimaryBorder }}
                    />

                    <div className="relative flex items-center gap-3">
                        <div
                            className="flex size-11 items-center justify-center rounded-md text-xl"
                            style={{
                                background: themeToken.colorPrimary,
                                color: themeToken.colorTextLightSolid,
                                boxShadow: themeToken.boxShadowTertiary,
                            }}
                        >
                            <SafetyCertificateOutlined />
                        </div>
                        <div>
                            <div className="text-lg font-semibold tracking-wide">{APP_NAME}</div>
                            <div
                                className="mt-0.5 text-[10px] tracking-[0.2em]"
                                style={{ color: themeToken.colorTextSecondary }}
                            >
                                INDUSTRIAL IOT PLATFORM
                            </div>
                        </div>
                    </div>

                    <div className="relative my-auto max-w-[480px]">
                        <div
                            className="mb-5 inline-flex items-center rounded border px-3 py-1.5 text-xs font-medium tracking-wider"
                            style={{
                                background: themeToken.colorPrimaryBg,
                                borderColor: themeToken.colorPrimaryBorder,
                                color: themeToken.colorPrimaryText,
                            }}
                        >
                            企业级设备运营控制台
                        </div>
                        <h1 className="m-0 text-[38px] font-semibold leading-[1.25] tracking-tight">
                            连接设备，沉淀数据
                            <br />
                            驱动生产决策
                        </h1>
                        <p
                            className="mb-8 mt-5 max-w-[440px] text-[15px] leading-7"
                            style={{ color: themeToken.colorTextSecondary }}
                        >
                            统一管理工业设备、通信链路、协议配置与组织权限，为生产现场提供清晰、可靠的数字化底座。
                        </p>
                        <div className="grid grid-cols-3 gap-3">
                            {[
                                { icon: <HddOutlined />, label: '设备接入' },
                                { icon: <ClusterOutlined />, label: '协议配置' },
                                { icon: <SafetyCertificateOutlined />, label: '权限治理' },
                            ].map((item) => (
                                <div
                                    key={item.label}
                                    className="rounded-md border px-3 py-4"
                                    style={{
                                        background: themeToken.colorBgContainer,
                                        borderColor: themeToken.colorBorderSecondary,
                                    }}
                                >
                                    <div
                                        className="text-lg"
                                        style={{ color: themeToken.colorPrimary }}
                                    >
                                        {item.icon}
                                    </div>
                                    <div
                                        className="mt-2 text-xs font-medium"
                                        style={{ color: themeToken.colorText }}
                                    >
                                        {item.label}
                                    </div>
                                </div>
                            ))}
                        </div>
                    </div>

                    <div
                        className="relative text-xs"
                        style={{ color: themeToken.colorTextDescription }}
                    >
                        稳定 · 安全 · 可追溯
                    </div>
                </section>

                <section
                    className="flex min-h-[560px] items-center px-7 py-12 sm:px-14 lg:min-h-[620px] lg:px-16"
                    style={{ background: themeToken.colorBgContainer }}
                >
                    <div className="mx-auto w-full max-w-[380px]">
                        <div className="mb-8 flex items-center gap-3 lg:hidden">
                            <div
                                className="flex size-10 items-center justify-center rounded-md text-lg"
                                style={{
                                    background: themeToken.colorPrimaryBg,
                                    color: themeToken.colorPrimary,
                                }}
                            >
                                <SafetyCertificateOutlined />
                            </div>
                            <div>
                                <div
                                    className="font-semibold"
                                    style={{ color: themeToken.colorText }}
                                >
                                    {APP_NAME}
                                </div>
                                <div
                                    className="text-[10px] tracking-[0.16em]"
                                    style={{ color: themeToken.colorTextDescription }}
                                >
                                    INDUSTRIAL IOT
                                </div>
                            </div>
                        </div>

                        <div className="mb-8">
                            <div
                                className="mb-3 h-1 w-9 rounded-full"
                                style={{ background: themeToken.colorPrimary }}
                            />
                            <h2
                                className="m-0 text-[28px] font-semibold tracking-tight"
                                style={{ color: themeToken.colorTextHeading }}
                            >
                                欢迎登录
                            </h2>
                            <p
                                className="mb-0 mt-2 text-sm"
                                style={{ color: themeToken.colorTextSecondary }}
                            >
                                使用企业账号进入设备运营控制台
                            </p>
                        </div>

                        <Form<Auth.LoginRequest>
                            form={form}
                            layout="vertical"
                            onFinish={onFinish}
                            autoComplete="off"
                        >
                            <Form.Item label="用户名" name="username">
                                <Input
                                    size="large"
                                    prefix={<UserOutlined />}
                                    placeholder="请输入用户名"
                                    autoComplete="off"
                                    name="login-username"
                                />
                            </Form.Item>
                            <Form.Item label="密码" name="password">
                                <Input.Password
                                    size="large"
                                    prefix={<LockOutlined />}
                                    placeholder="请输入密码"
                                    autoComplete="new-password"
                                    name="login-password"
                                />
                            </Form.Item>
                            <Form.Item style={{ marginBottom: 0, marginTop: 12 }}>
                                <Button
                                    type="primary"
                                    htmlType="submit"
                                    block
                                    size="large"
                                    loading={mutation.isPending}
                                    disabled={mutation.isPending}
                                >
                                    登录系统
                                </Button>
                            </Form.Item>
                        </Form>
                        <div
                            className="mt-8 border-t pt-5 text-xs leading-5"
                            style={{
                                borderColor: themeToken.colorBorderSecondary,
                                color: themeToken.colorTextDescription,
                            }}
                        >
                            为保障企业数据安全，请勿在公共设备上保存登录信息。
                        </div>
                    </div>
                </section>
            </div>
        </main>
    );
}

export default LoginPage;
