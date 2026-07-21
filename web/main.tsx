import { StyleProvider } from '@ant-design/cssinjs';
import { App, ConfigProvider } from 'antd';
import type { ThemeConfig } from 'antd';
import zhCN from 'antd/es/locale/zh_CN';
import ReactDOM from 'react-dom/client';
import { ErrorBoundary } from './components/ErrorBoundary';
import { Message } from './providers/Message';
import { TanstackQuery } from './providers/TanstackQuery';
import { AppRoutes } from './routes';
import { APP_NAME } from './config/app';
import './styles/index.css';

const rootElement = document.getElementById('root');
if (!rootElement) throw new Error('Root element #root not found in DOM');

document.title = APP_NAME;

const loadingElement = document.getElementById('app-loading-text');
if (loadingElement) {
    loadingElement.textContent = `${APP_NAME} 正在加载`;
}

const enterpriseTheme: ThemeConfig = {
    token: {
        colorPrimary: '#1d5fd1',
        colorInfo: '#1d5fd1',
        colorSuccess: '#16865c',
        colorWarning: '#d97706',
        colorError: '#d14343',
        colorText: '#172033',
        colorTextSecondary: '#667085',
        colorBorder: '#d8e0ea',
        colorBorderSecondary: '#e8edf3',
        colorBgLayout: '#eef2f6',
        colorBgContainer: '#ffffff',
        borderRadius: 6,
        borderRadiusLG: 10,
        controlHeight: 36,
        fontSize: 14,
        boxShadowSecondary: '0 12px 32px rgba(15, 31, 54, 0.14)',
    },
    components: {
        Button: {
            fontWeight: 500,
            defaultShadow: 'none',
            primaryShadow: 'none',
            dangerShadow: 'none',
        },
        Card: {
            headerBg: '#ffffff',
            bodyPadding: 20,
        },
        Form: {
            labelColor: '#344054',
            labelFontSize: 13,
            itemMarginBottom: 20,
            verticalLabelPadding: '0 0 6px',
        },
        Layout: {
            bodyBg: '#eef2f6',
            headerBg: '#ffffff',
            headerHeight: 60,
            headerPadding: '0 20px',
            siderBg: '#10233f',
        },
        Menu: {
            darkItemBg: '#10233f',
            darkSubMenuItemBg: '#0c1c33',
            darkItemColor: '#b9c6d8',
            darkItemHoverBg: '#17345c',
            darkItemHoverColor: '#ffffff',
            darkItemSelectedBg: '#1d5fd1',
            darkItemSelectedColor: '#ffffff',
            itemBorderRadius: 6,
            itemMarginInline: 10,
            itemHeight: 42,
        },
        Modal: {
            titleColor: '#172033',
            titleFontSize: 17,
            headerBg: '#ffffff',
            contentBg: '#ffffff',
            footerBg: '#ffffff',
        },
        Table: {
            headerBg: '#f4f7fa',
            headerColor: '#344054',
            headerBorderRadius: 6,
            borderColor: '#e8edf3',
            rowHoverBg: '#f7faff',
            cellPaddingBlock: 13,
            cellPaddingInline: 16,
        },
    },
};

ReactDOM.createRoot(rootElement).render(
    <ErrorBoundary>
        <StyleProvider hashPriority="low" layer>
            <ConfigProvider locale={zhCN} theme={enterpriseTheme}>
                <App>
                    <Message />
                    <TanstackQuery>
                        <AppRoutes />
                    </TanstackQuery>
                </App>
            </ConfigProvider>
        </StyleProvider>
    </ErrorBoundary>
);
