/**
 * 将 Ant Design App 上下文中的 message 实例暴露给非组件代码（如 Axios 拦截器）
 * 必须在 <App> 内部渲染
 */

import { App } from 'antd';
import type { MessageInstance } from 'antd/es/message/interface';

let messageApi: MessageInstance | undefined;

export function getMessageInstance(): MessageInstance | undefined {
    return messageApi;
}

export function MessageContextBridge() {
    const { message } = App.useApp();
    messageApi = message;
    return null;
}
