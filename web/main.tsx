import { StyleProvider } from '@ant-design/cssinjs';
import { App, ConfigProvider } from 'antd';
import zhCN from 'antd/es/locale/zh_CN';
import ReactDOM from 'react-dom/client';
import { ErrorBoundary } from './components/ErrorBoundary';
import { APP_NAME } from './config/app';
import { MessageContextBridge } from './providers/MessageContextBridge';
import { TanStackQueryProvider } from './providers/TanStackQueryProvider';
import { AppRoutes } from './routes';
import './styles/index.css';

const rootElement = document.getElementById('root');
if (!rootElement) throw new Error('Root element #root not found in DOM');

document.title = APP_NAME;

const loadingElement = document.getElementById('app-loading-text');
if (loadingElement) {
    loadingElement.textContent = `${APP_NAME} 正在加载`;
}

ReactDOM.createRoot(rootElement).render(
    <ErrorBoundary>
        <StyleProvider hashPriority="low" layer>
            <ConfigProvider locale={zhCN}>
                <App>
                    <MessageContextBridge />
                    <TanStackQueryProvider>
                        <AppRoutes />
                    </TanStackQueryProvider>
                </App>
            </ConfigProvider>
        </StyleProvider>
    </ErrorBoundary>
);
