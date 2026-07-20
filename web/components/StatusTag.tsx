/**
 * 状态标签组件
 * 用于统一显示启用/禁用状态
 */

import { Tag, type TagProps } from 'antd';

interface StatusTagProps {
    status: 'enabled' | 'disabled';
    enabledText?: string;
    disabledText?: string;
    enabledColor?: TagProps['color'];
    disabledColor?: TagProps['color'];
}

/**
 * 状态标签组件
 */
export function StatusTag({
    status,
    enabledText = '启用',
    disabledText = '禁用',
    enabledColor = 'success',
    disabledColor = 'default',
}: StatusTagProps) {
    return status === 'enabled' ? (
        <Tag color={enabledColor}>{enabledText}</Tag>
    ) : (
        <Tag color={disabledColor}>{disabledText}</Tag>
    );
}
