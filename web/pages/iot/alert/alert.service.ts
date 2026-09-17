import { useQuery, useQueryClient, type UseQueryOptions } from '@tanstack/react-query';
import { useCallback } from 'react';
import { getDeviceOptions } from '@/pages/iot/device/device.service';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import type { PaginatedResult } from '@/types/pagination';
import * as alertApi from './alert.api';
import type { Alert } from './alert.types';
export const alertKeys = {
    ruleLists: ['alerts', 'rules'] as const,
    templateQueries: ['alerts', 'templates'] as const,
    rules: (params?: Record<string, unknown>) => ['alerts', 'rules', params] as const,
    records: (params?: Record<string, unknown>) => ['alerts', 'records', params] as const,
    templates: (params?: Record<string, unknown>) => ['alerts', 'templates', params] as const,
    templateDetail: (id: string) => ['alerts', 'templates', 'detail', id] as const,
    stats: (params?: Record<string, unknown>) => ['alerts', 'stats', params] as const,
};
export function useAlertTemplateLoader() {
    const queryClient = useQueryClient();
    return useCallback(
        (id: string) =>
            queryClient.fetchQuery({
                queryKey: alertKeys.templateDetail(id),
                queryFn: ({ signal }) => alertApi.getTemplateDetail(id, signal),
                staleTime: 0,
            }),
        [queryClient]
    );
}
export function useAlertRuleList(params?: Record<string, unknown>) {
    return useQuery({
        queryKey: alertKeys.rules(params),
        queryFn: ({ signal }) => alertApi.getRules(params, signal),
        refetchInterval: false,
    });
}
export function useAlertRecordList(
    params?: Record<string, unknown>,
    options?: Omit<UseQueryOptions<PaginatedResult<Alert.RecordItem>>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: alertKeys.records(params),
        queryFn: () => alertApi.getRecords(params),
        ...options,
    });
}
export function useAlertTemplateList(params?: Record<string, unknown>) {
    return useQuery({
        queryKey: alertKeys.templates(params),
        queryFn: ({ signal }) => alertApi.getTemplates(params, signal),
        refetchInterval: false,
    });
}
export function useAlertStats(
    params?: Record<string, unknown>,
    options?: Omit<UseQueryOptions<Alert.ActiveStats>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: alertKeys.stats(params),
        queryFn: () => alertApi.getStats(params),
        ...options,
    });
}
export function useAlertRuleSave() {
    return useSaveMutation<
        Alert.RuleDto & {
            id?: string;
        },
        Alert.RuleDto,
        Alert.RuleDto
    >({
        createFn: alertApi.createRule,
        updateFn: alertApi.updateRule,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [alertKeys.ruleLists],
    });
}
export function useAlertRuleDelete() {
    return useMutationWithMessage({
        mutationFn: alertApi.deleteRule,
        successMessage: '删除成功',
        invalidateKeys: [alertKeys.ruleLists],
    });
}
export function useAlertRuleBatchDelete() {
    return useMutationWithMessage({
        mutationFn: alertApi.batchDeleteRules,
        successMessage: '批量删除成功',
        invalidateKeys: [alertKeys.ruleLists],
    });
}
export function useAlertTemplateSave() {
    return useSaveMutation<
        Alert.TemplateDto & {
            id?: string;
        },
        Alert.TemplateDto,
        Alert.TemplateDto
    >({
        createFn: alertApi.createTemplate,
        updateFn: alertApi.updateTemplate,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [alertKeys.templateQueries],
    });
}
export function useAlertTemplateDelete() {
    return useMutationWithMessage({
        mutationFn: alertApi.deleteTemplate,
        successMessage: '删除成功',
        invalidateKeys: [alertKeys.templateQueries],
    });
}
export function useAlertApplyTemplate() {
    return useMutationWithMessage({
        mutationFn: alertApi.applyTemplate,
        successMessage: (result) => `应用成功，已创建 ${result.success} 条规则`,
        invalidateKeys: [alertKeys.ruleLists],
    });
}
export function useAlertAcknowledge() {
    return useMutationWithMessage({
        mutationFn: alertApi.acknowledgeRecord,
        successMessage: '确认成功',
    });
}
export function useAlertBatchAcknowledge() {
    return useMutationWithMessage({
        mutationFn: alertApi.batchAcknowledge,
        successMessage: '批量确认成功',
    });
}
export function useDeviceOptions(options?: { enabled?: boolean }) {
    return useQuery({
        queryKey: ['devices', 'options'],
        queryFn: ({ signal }) => getDeviceOptions(signal),
        enabled: options?.enabled ?? true,
        refetchInterval: false,
    });
}
