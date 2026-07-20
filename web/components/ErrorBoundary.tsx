import { Button, Result } from 'antd';
import { Component, type ErrorInfo, type ReactNode } from 'react';
import { getAppTitle } from '@/config/app';

interface ErrorBoundaryProps {
    children: ReactNode;
}

interface ErrorBoundaryState {
    hasError: boolean;
    error: Error | null;
}

export class ErrorBoundary extends Component<ErrorBoundaryProps, ErrorBoundaryState> {
    state: ErrorBoundaryState = { hasError: false, error: null };

    static getDerivedStateFromError(error: Error): ErrorBoundaryState {
        return { hasError: true, error };
    }

    componentDidCatch(error: Error, info: ErrorInfo) {
        console.error('[ErrorBoundary]', error, info.componentStack);
    }

    handleReset = () => {
        this.setState({ hasError: false, error: null });
    };

    handleReload = () => {
        window.location.reload();
    };

    render() {
        if (this.state.hasError) {
            return (
                <div
                    style={{
                        display: 'flex',
                        justifyContent: 'center',
                        alignItems: 'center',
                        minHeight: '100vh',
                    }}
                >
                    <Result
                        status="error"
                        title={getAppTitle('页面出现异常')}
                        subTitle={this.state.error?.message || '未知错误'}
                        extra={[
                            <Button key="retry" type="primary" onClick={this.handleReset}>
                                重试
                            </Button>,
                            <Button key="reload" onClick={this.handleReload}>
                                刷新页面
                            </Button>,
                        ]}
                    />
                </div>
            );
        }

        return this.props.children;
    }
}
