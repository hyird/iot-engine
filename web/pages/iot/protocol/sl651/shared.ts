/**
 * SL651 协议配置 - 共享类型和常量
 */

import type { useProtocolConfigSave } from '../protocol.service';
import type { SL651 } from '../protocol.types';

/** 编码类型列表 */
export const EncodeList: SL651.EncodeType[] = ['BCD', 'TIME_YYMMDDHHMMSS', 'JPEG', 'DICT', 'HEX'];

/** 生成唯一 ID（兼容非安全上下文） */
export const generateId = (): string =>
    '10000000-1000-4000-8000-100000000000'.replace(/[018]/g, (c) =>
        (+c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
    );

/** SaveMutation 类型（避免每个 Modal 重复定义） */
export type SaveMutation = ReturnType<typeof useProtocolConfigSave>;

/** 表单中的条件数据（可能不完整） */
export interface FormCondition {
    bitIndex?: string;
    bitValue?: string;
}

/** 表单中的映射项数据（可能不完整） */
export interface FormMapItem {
    key?: string;
    label?: string;
    value?: string;
    dependsOn?: {
        operator?: 'AND' | 'OR';
        conditions?: FormCondition[];
    };
}
