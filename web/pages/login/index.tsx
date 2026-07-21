/**
 * 认证
 */

export { useCurrentUser, useLogout } from './login.service';

import { useMutation } from '@tanstack/react-query';
import {
    ClusterOutlined,
    HddOutlined,
    LockOutlined,
    SafetyCertificateOutlined,
    UserOutlined,
} from '@ant-design/icons';
import { Button, Form, Input } from 'antd';
import { useEffect } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { APP_NAME, getAppTitle } from '@/config/app';
import { login } from './login.api';
import { loginSchema } from './login.schema';
import { useAuthStore } from '@/store/authStore';
import { validateForm } from '@/utils/validation';
import type { Auth } from './login.types';

interface LocationState {
    from?: {
        pathname: string;
    };
}

const pageTitle = getAppTitle('登录系统');

const brandPanelStyle = {
    background:
        'linear-gradient(145deg, rgba(9, 31, 58, 0.98) 0%, rgba(17, 59, 103, 0.98) 58%, rgba(20, 73, 126, 0.96) 100%)',
};

const gridPatternStyle = {
    backgroundImage:
        'linear-gradient(rgba(255,255,255,0.04) 1px, transparent 1px), linear-gradient(90deg, rgba(255,255,255,0.04) 1px, transparent 1px)',
    backgroundSize: '40px 40px',
    maskImage: 'linear-gradient(to bottom right, black, transparent 78%)',
};

function LoginPage() {
    const [form] = Form.useForm<Auth.LoginRequest>();
    const navigate = useNavigate();
    const location = useLocation();
    const { token, setAuth } = useAuthStore();

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
        <main className="flex min-h-screen items-center justify-center overflow-hidden bg-slate-100 p-4 sm:p-8">
            <div className="grid w-full max-w-[1120px] overflow-hidden rounded-xl border border-slate-200 bg-white shadow-[0_24px_64px_rgba(15,31,54,0.14)] lg:grid-cols-[1.08fr_0.92fr]">
                <section
                    className="relative hidden min-h-[620px] flex-col overflow-hidden p-12 text-white lg:flex"
                    style={brandPanelStyle}
                >
                    <div className="absolute inset-0" style={gridPatternStyle} aria-hidden="true" />
                    <div className="absolute -right-20 top-28 size-72 rounded-full border border-blue-300/15" />
                    <div className="absolute -right-6 top-44 size-44 rounded-full border border-blue-300/15" />

                    <div className="relative flex items-center gap-3">
                        <div className="flex size-11 items-center justify-center rounded-md bg-blue-500 text-xl shadow-lg shadow-blue-950/30">
                            <SafetyCertificateOutlined />
                        </div>
                        <div>
                            <div className="text-lg font-semibold tracking-wide">{APP_NAME}</div>
                            <div className="mt-0.5 text-[10px] tracking-[0.2em] text-blue-200">
                                INDUSTRIAL IOT PLATFORM
                            </div>
                        </div>
                    </div>

                    <div className="relative my-auto max-w-[480px]">
                        <div className="mb-5 inline-flex items-center rounded border border-blue-300/25 bg-blue-400/10 px-3 py-1.5 text-xs font-medium tracking-wider text-blue-100">
                            企业级设备运营控制台
                        </div>
                        <h1 className="m-0 text-[38px] font-semibold leading-[1.25] tracking-tight">
                            连接设备，沉淀数据
                            <br />
                            驱动生产决策
                        </h1>
                        <p className="mb-8 mt-5 max-w-[440px] text-[15px] leading-7 text-slate-300">
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
                                    className="rounded-md border border-white/10 bg-white/[0.06] px-3 py-4"
                                >
                                    <div className="text-lg text-blue-300">{item.icon}</div>
                                    <div className="mt-2 text-xs font-medium text-slate-200">
                                        {item.label}
                                    </div>
                                </div>
                            ))}
                        </div>
                    </div>

                    <div className="relative text-xs text-slate-400">稳定 · 安全 · 可追溯</div>
                </section>

                <section className="flex min-h-[560px] items-center bg-white px-7 py-12 sm:px-14 lg:min-h-[620px] lg:px-16">
                    <div className="mx-auto w-full max-w-[380px]">
                        <div className="mb-8 flex items-center gap-3 lg:hidden">
                            <div className="flex size-10 items-center justify-center rounded-md bg-blue-50 text-lg text-blue-700">
                                <SafetyCertificateOutlined />
                            </div>
                            <div>
                                <div className="font-semibold text-slate-900">{APP_NAME}</div>
                                <div className="text-[10px] tracking-[0.16em] text-slate-400">
                                    INDUSTRIAL IOT
                                </div>
                            </div>
                        </div>

                        <div className="mb-8">
                            <div className="mb-3 h-1 w-9 rounded-full bg-blue-600" />
                            <h2 className="m-0 text-[28px] font-semibold tracking-tight text-slate-900">
                                欢迎登录
                            </h2>
                            <p className="mb-0 mt-2 text-sm text-slate-500">
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
                        <div className="mt-8 border-t border-slate-100 pt-5 text-xs leading-5 text-slate-400">
                            为保障企业数据安全，请勿在公共设备上保存登录信息。
                        </div>
                    </div>
                </section>
            </div>
        </main>
    );
}

export default LoginPage;
