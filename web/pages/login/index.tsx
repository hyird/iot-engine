/**
 * 认证
 */

export { useCurrentUser, useLogout } from './login.service';

import { useMutation } from '@tanstack/react-query';
import { LockOutlined, SafetyCertificateOutlined, UserOutlined } from '@ant-design/icons';
import { Button, Card, Form, Input, Typography } from 'antd';
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

const { Title } = Typography;
const pageTitle = getAppTitle('登录系统');

const containerStyle = {
    minHeight: '100vh',
    width: '100%',
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    padding: 'clamp(24px, 4vw, 48px)',
    background: 'linear-gradient(180deg, #fbfdff 0%, #eef4ff 48%, #f8fbff 100%)',
    position: 'relative' as const,
    overflow: 'hidden' as const,
};

const orbStyle = {
    position: 'absolute' as const,
    borderRadius: '999px',
    filter: 'blur(30px)',
    pointerEvents: 'none' as const,
    opacity: 0.74,
};

const orbOneStyle = {
    ...orbStyle,
    width: 520,
    height: 520,
    top: -180,
    left: -180,
    background:
        'radial-gradient(circle, rgba(59, 130, 246, 0.18), rgba(59, 130, 246, 0.04) 64%, transparent 76%)',
};

const orbTwoStyle = {
    ...orbStyle,
    width: 620,
    height: 620,
    right: -220,
    bottom: -240,
    background:
        'radial-gradient(circle, rgba(37, 99, 235, 0.12), rgba(37, 99, 235, 0.03) 66%, transparent 78%)',
};

const haloStyle = {
    position: 'absolute' as const,
    inset: '10% 12% auto auto',
    width: '38vw',
    height: '38vw',
    maxWidth: 460,
    maxHeight: 460,
    minWidth: 300,
    minHeight: 300,
    borderRadius: '50%',
    background:
        'radial-gradient(circle, rgba(255, 255, 255, 0.82) 0%, rgba(255, 255, 255, 0.26) 40%, rgba(255, 255, 255, 0) 72%)',
    opacity: 0.84,
    filter: 'blur(14px)',
};

const sweepStyle = {
    position: 'absolute' as const,
    inset: 0,
    pointerEvents: 'none' as const,
    opacity: 0.72,
    background:
        'linear-gradient(135deg, rgba(255, 255, 255, 0) 0%, rgba(255, 255, 255, 0.16) 42%, rgba(255, 255, 255, 0.3) 52%, rgba(255, 255, 255, 0) 68%)',
    maskImage: 'radial-gradient(circle at center, black 0%, transparent 76%)',
};

const ribbonStyle = {
    position: 'absolute' as const,
    inset: '12% auto auto 9%',
    width: '52vw',
    height: '64vh',
    maxWidth: 720,
    maxHeight: 660,
    borderRadius: '48% 52% 44% 56% / 42% 35% 65% 58%',
    background:
        'linear-gradient(145deg, rgba(255, 255, 255, 0.16) 0%, rgba(191, 219, 254, 0.22) 42%, rgba(191, 219, 254, 0.02) 100%)',
    opacity: 0.52,
    filter: 'blur(3px)',
    transform: 'rotate(-12deg)',
};

const shellStyle = {
    position: 'relative' as const,
    zIndex: 1,
    width: 'min(440px, 100%)',
};

const loginCardStyle = {
    borderRadius: 18,
    boxShadow: '0 18px 44px rgba(15, 23, 42, 0.08)',
    border: '1px solid rgba(148, 163, 184, 0.14)',
    overflow: 'hidden' as const,
    background: 'rgba(255, 255, 255, 0.94)',
    backdropFilter: 'blur(12px)',
    animation: 'cardEnter 620ms cubic-bezier(0.22, 1, 0.36, 1) both',
};

const cardBodyStyle = {
    padding: 'clamp(28px, 4vw, 36px)',
};

const brandRowStyle = {
    display: 'flex',
    alignItems: 'center',
    gap: 12,
    marginBottom: 20,
};

const logoMarkStyle = {
    display: 'inline-flex',
    alignItems: 'center',
    justifyContent: 'center',
    width: 42,
    height: 42,
    borderRadius: 12,
    background: '#eff6ff',
    border: '1px solid rgba(96, 165, 250, 0.18)',
    color: '#2563eb',
};

const brandMetaStyle = {
    fontSize: 13,
    color: '#64748b',
    letterSpacing: '0.08em',
    textTransform: 'uppercase' as const,
};

const titleStyle = {
    marginBottom: 4,
    color: '#0f172a',
};

const formLabelStyle = {
    fontWeight: 600,
};

const responsiveStyle = `
  @keyframes cardEnter {
    from {
      opacity: 0;
      transform: translateY(16px) scale(0.985);
    }
    to {
      opacity: 1;
      transform: translateY(0);
    }
  }

  @media (max-width: 640px) {
    .login-shell {
      width: 100%;
    }

    .login-card {
      border-radius: 18px;
    }
  }
`;

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
        <div style={containerStyle}>
            <style>{responsiveStyle}</style>
            <div style={sweepStyle} aria-hidden="true" />
            <div style={ribbonStyle} aria-hidden="true" />
            <div style={haloStyle} aria-hidden="true" />
            <div style={orbOneStyle} aria-hidden="true" />
            <div style={orbTwoStyle} aria-hidden="true" />
            <div style={shellStyle} className="login-shell">
                <Card
                    style={loginCardStyle}
                    styles={{ body: cardBodyStyle }}
                    className="login-card"
                >
                    <div style={brandRowStyle}>
                        <div style={logoMarkStyle}>
                            <SafetyCertificateOutlined />
                        </div>
                        <div>
                            <div style={brandMetaStyle}>{APP_NAME}</div>
                            <Title level={3} style={titleStyle}>
                                登录系统
                            </Title>
                        </div>
                    </div>

                    <Form<Auth.LoginRequest>
                        form={form}
                        layout="vertical"
                        onFinish={onFinish}
                        autoComplete="off"
                    >
                        <Form.Item
                            label={<span style={formLabelStyle}>用户名</span>}
                            name="username"
                        >
                            <Input
                                prefix={<UserOutlined />}
                                placeholder="请输入用户名"
                                autoComplete="off"
                                name="login-username"
                            />
                        </Form.Item>
                        <Form.Item label={<span style={formLabelStyle}>密码</span>} name="password">
                            <Input.Password
                                prefix={<LockOutlined />}
                                placeholder="请输入密码"
                                autoComplete="new-password"
                                name="login-password"
                            />
                        </Form.Item>
                        <Form.Item style={{ marginBottom: 0, marginTop: 8 }}>
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
                </Card>
            </div>
        </div>
    );
}

export default LoginPage;
