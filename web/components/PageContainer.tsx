import type { ReactNode } from 'react';

interface PageContainerProps {
    /** 页面标题 */
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
export function PageContainer({ title, header, children, footer }: PageContainerProps) {
    return (
        <section className="flex h-full flex-col overflow-hidden bg-white">
            {(title || header) && (
                <div className="shrink-0 border-b border-slate-200/80 px-4 py-4 sm:px-5">
                    {title && (
                        <div>
                            <h1 className="m-0 text-lg font-semibold tracking-tight text-slate-900">
                                {title}
                            </h1>
                            <div className="mt-2 h-0.5 w-8 rounded-full bg-blue-600" />
                        </div>
                    )}
                    {header && <div className={title ? 'mt-4' : ''}>{header}</div>}
                </div>
            )}
            <div className="min-h-0 flex-1 overflow-hidden px-4 py-4 sm:px-5">{children}</div>
            {footer && (
                <div className="shrink-0 border-t border-slate-200/80 px-4 py-3 sm:px-5">
                    {footer}
                </div>
            )}
        </section>
    );
}
