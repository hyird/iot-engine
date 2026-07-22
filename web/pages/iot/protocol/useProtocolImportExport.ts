/**
 * 协议配置导入导出 Hook
 * 支持 SL651、Modbus 和 S7 配置的 JSON 导入导出
 */

import { useQueryClient } from '@tanstack/react-query';
import { App } from 'antd';
import { useCallback, useEffect, useRef, useState } from 'react';
import * as protocolApi from './protocol.api';
import { protocolCreateSchema } from './protocol.schema';
import { protocolQueryKeys } from './protocol.service';
import type { Protocol } from './protocol.types';

const MAX_NAME_LENGTH = 64;

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

    let index = 1;
    while (true) {
        const suffix = index === 1 ? ' (导入)' : ` (导入) ${index}`;
        const baseName = name.slice(0, Math.max(0, MAX_NAME_LENGTH - suffix.length)).trimEnd();
        const candidate = `${baseName}${suffix}`;
        if (!existingNames.has(candidate)) return candidate;
        index++;
    }
}

export function useProtocolImportExport(protocol: Protocol.Type) {
    const { message } = App.useApp();
    const queryClient = useQueryClient();
    const fileInputRef = useRef<HTMLInputElement | null>(null);
    const [exporting, setExporting] = useState(false);
    const [importing, setImporting] = useState(false);

    /** 导出当前协议的所有配置 */
    const exportConfigs = useCallback(async () => {
        setExporting(true);
        try {
            const configs = await protocolApi.getAll({ protocol }, { _silent: true });
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
            a.style.display = 'none';
            document.body.appendChild(a);
            a.click();
            a.remove();
            window.setTimeout(() => URL.revokeObjectURL(url), 1000);

            message.success(`已导出 ${exportData.length} 条配置`);
        } catch (error) {
            const reason = error instanceof Error ? error.message : '未知错误';
            message.error(`导出失败：${reason}`);
        } finally {
            setExporting(false);
        }
    }, [protocol, message]);

    /** 处理导入文件 */
    const processImport = useCallback(
        async (file: File) => {
            setImporting(true);
            try {
                const text = await file.text();
                let rawItems: unknown;

                try {
                    rawItems = JSON.parse(text);
                } catch {
                    message.error('JSON 格式错误');
                    return;
                }

                if (!Array.isArray(rawItems) || rawItems.length === 0) {
                    message.error('文件内容为空或格式不正确');
                    return;
                }

                const items: Protocol.CreateDto[] = [];
                for (let i = 0; i < rawItems.length; i++) {
                    const rawItem = rawItems[i];
                    if (typeof rawItem !== 'object' || rawItem === null || Array.isArray(rawItem)) {
                        message.error(`第 ${i + 1} 项必须是对象`);
                        return;
                    }

                    const itemProtocol = (rawItem as Record<string, unknown>).protocol;
                    if (itemProtocol !== protocol) {
                        const actualProtocol =
                            typeof itemProtocol === 'string' ? itemProtocol : '未指定';
                        message.error(
                            `第 ${i + 1} 项协议类型为 ${actualProtocol}，不能导入到 ${protocol} 页面`
                        );
                        return;
                    }

                    const parsedItem = protocolCreateSchema.safeParse(rawItem);
                    if (!parsedItem.success) {
                        const issue = parsedItem.error.issues[0];
                        const path = issue.path.length
                            ? `${issue.path.map(String).join('.')}：`
                            : '';
                        message.error(`第 ${i + 1} 项 ${path}${issue.message}`);
                        return;
                    }

                    items.push(parsedItem.data);
                }

                // 数据库按全协议范围约束名称唯一，必须加载全部协议名称后再处理冲突。
                const existingList = await protocolApi.getAll(undefined, { _silent: true });
                const existingNames = new Set(existingList.map((config) => config.name));

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
                        await protocolApi.create(
                            {
                                ...item,
                                protocol,
                                name: finalName,
                            },
                            { _silent: true }
                        );
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
            } catch (error) {
                const reason = error instanceof Error ? error.message : '未知错误';
                message.error(`导入失败：${reason}`);
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
            document.body.appendChild(input);
            fileInputRef.current = input;
        }
        fileInputRef.current.onchange = (event) => {
            const file = (event.target as HTMLInputElement).files?.[0];
            if (file) processImport(file);
        };
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

    return { exportConfigs, triggerImport, exporting, importing };
}
