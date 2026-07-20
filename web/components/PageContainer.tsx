import type { ReactNode } from 'react';

interface PageContainerProps {
    /** 页面标题（可选，暂未使用，保留接口） */
    title?: string;
    /** 页面标题区域（搜索栏、操作按钮等） - 固定不滚动 */
    header?: ReactNode;
    /** 页面主体内容 - 可滚动 */
    children: ReactNode;
    /** 页面底部区域（如分页） - 固定不滚动 */
    footer?: ReactNode;
}

/**
 * 页面容器组件
 * - header: 固定在顶部的搜索/操作栏
 * - children: 可滚动的主体内容
 */
export function PageContainer({ header, children, footer }: PageContainerProps) {
    return (
        <div className="flex h-full flex-col overflow-hidden p-4">
            {header && <div className="shrink-0 pb-4">{header}</div>}
            <div className="flex-1 overflow-x-hidden overflow-y-auto">{children}</div>
            {footer && <div className="shrink-0 pt-4">{footer}</div>}
        </div>
    );
}
