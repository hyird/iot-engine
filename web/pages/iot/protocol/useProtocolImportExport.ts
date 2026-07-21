/**
 * 协议配置导入导出 Hook
 * 支持 SL651、Modbus 和 S7 配置的 JSON 导入导出
 */

import { useQueryClient } from '@tanstack/react-query';
import { App } from 'antd';
import { useCallback, useEffect, useRef, useState } from 'react';
import * as protocolApi from './protocol.api';
import { protocolQueryKeys } from './protocol.service';
import type { Protocol } from './protocol.types';

/** 导出配置项（不含 id/时间戳） */
interface ExportItem {
    protocol: Protocol.Type;
    name: string;
    enabled: boolean;
    config: Protocol.Item['config'];
    remark?: string;
}

/** 导入结果 */
interface ImportResult {
    total: number;
    success: number;
    renamed: string[];
    failed: { name: string; reason: string }[];
}

/** 生成不冲突的名称 */
function resolveNameConflict(name: string, existingNames: Set<string>): string {
    if (!existingNames.has(name)) return name;

    const suffix = ' (导入)';
    const candidate = name + suffix;
    if (!existingNames.has(candidate)) return candidate;

    let i = 2;
    while (existingNames.has(`${name}${suffix} ${i}`)) i++;
    return `${name}${suffix} ${i}`;
}

export function useProtocolImportExport(protocol: Protocol.Type) {
    const { message } = App.useApp();
    const queryClient = useQueryClient();
    const fileInputRef = useRef<HTMLInputElement | null>(null);
    const [importing, setImporting] = useState(false);

    /** 导出当前协议的所有配置 */
    const exportConfigs = useCallback(
        (configs: Protocol.Item[]) => {
            if (!configs.length) {
                message.warning('没有可导出的配置');
                return;
            }

            const exportData: ExportItem[] = configs.map(
                ({ id: _id, created_at: _c, updated_at: _u, ...rest }) => rest
            );

            const json = JSON.stringify(exportData, null, 2);
            const blob = new Blob([json], { type: 'application/json' });
            const url = URL.createObjectURL(blob);

            const date = new Date().toISOString().slice(0, 10).replace(/-/g, '');
            const a = document.createElement('a');
            a.href = url;
            a.download = `${protocol}_configs_${date}.json`;
            a.click();
            URL.revokeObjectURL(url);

            message.success(`已导出 ${exportData.length} 条配置`);
        },
        [protocol, message]
    );

    /** 处理导入文件 */
    const processImport = useCallback(
        async (file: File) => {
            setImporting(true);
            try {
                const text = await file.text();
                let items: ExportItem[];

                try {
                    items = JSON.parse(text);
                } catch {
                    message.error('JSON 格式错误');
                    return;
                }

                if (!Array.isArray(items) || items.length === 0) {
                    message.error('文件内容为空或格式不正确');
                    return;
                }

                // 校验每项必要字段和 config 结构
                for (let i = 0; i < items.length; i++) {
                    const item = items[i];
                    if (!item.name || !item.config) {
                        message.error(`第 ${i + 1} 项缺少 name 或 config 字段`);
                        return;
                    }
                    if (typeof item.config !== 'object' || Array.isArray(item.config)) {
                        message.error(`第 ${i + 1} 项 config 必须是对象`);
                        return;
                    }
                    // 协议特定结构校验
                    const cfg = item.config as Record<string, unknown>;
                    if (protocol === 'SL651' && !Array.isArray(cfg.funcs)) {
                        message.error(`第 ${i + 1} 项缺少 config.funcs 数组（SL651 必需）`);
                        return;
                    }
                    if (
                        protocol === 'Modbus' &&
                        cfg.registers !== undefined &&
                        !Array.isArray(cfg.registers)
                    ) {
                        message.error(`第 ${i + 1} 项 config.registers 必须是数组`);
                        return;
                    }
                    if (protocol === 'S7' && cfg.areas !== undefined && !Array.isArray(cfg.areas)) {
                        message.error(`第 ${i + 1} 项 config.areas 必须是数组`);
                        return;
                    }
                }

                // 获取当前已有配置的名称
                const existingList = await protocolApi.getList({ protocol, pageSize: 999 });
                const existingNames = new Set(existingList.list.map((c) => c.name));

                const result: ImportResult = {
                    total: items.length,
                    success: 0,
                    renamed: [],
                    failed: [],
                };

                for (const item of items) {
                    const finalName = resolveNameConflict(item.name, existingNames);
                    if (finalName !== item.name) {
                        result.renamed.push(`${item.name} → ${finalName}`);
                    }

                    try {
                        await protocolApi.create({
                            protocol, // 强制使用当前页面协议类型，忽略文件中的 protocol
                            name: finalName,
                            enabled: item.enabled ?? true,
                            config: item.config,
                            remark: item.remark,
                        });
                        existingNames.add(finalName);
                        result.success++;
                    } catch (e) {
                        result.failed.push({
                            name: item.name,
                            reason: e instanceof Error ? e.message : '未知错误',
                        });
                    }
                }

                // 刷新缓存
                await queryClient.invalidateQueries({ queryKey: protocolQueryKeys.all });

                // 显示结果
                if (result.success === result.total) {
                    const renameInfo =
                        result.renamed.length > 0 ? `\n重命名：${result.renamed.join('、')}` : '';
                    message.success(`成功导入 ${result.success} 条配置${renameInfo}`);
                } else {
                    const failInfo = result.failed.map((f) => `${f.name}(${f.reason})`).join('、');
                    message.warning(
                        `导入完成：${result.success}/${result.total} 成功${failInfo ? `，失败：${failInfo}` : ''}`
                    );
                }
            } catch {
                message.error('导入失败，请检查文件格式');
            } finally {
                setImporting(false);
                // 重置文件输入，允许再次选择同一文件
                if (fileInputRef.current) fileInputRef.current.value = '';
            }
        },
        [protocol, message, queryClient]
    );

    /** 触发文件选择 */
    const triggerImport = useCallback(() => {
        if (!fileInputRef.current) {
            const input = document.createElement('input');
            input.type = 'file';
            input.accept = '.json';
            input.style.display = 'none';
            input.addEventListener('change', (e) => {
                const file = (e.target as HTMLInputElement).files?.[0];
                if (file) processImport(file);
            });
            document.body.appendChild(input);
            fileInputRef.current = input;
        }
        fileInputRef.current.click();
    }, [processImport]);

    // 组件卸载时清理动态创建的 input 元素
    useEffect(() => {
        return () => {
            if (fileInputRef.current) {
                fileInputRef.current.remove();
                fileInputRef.current = null;
            }
        };
    }, []);

    return { exportConfigs, triggerImport, importing };
}
