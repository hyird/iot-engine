import { useSnapshotQuery } from '@/hooks/useSnapshotQuery';
import type { UseQueryOptions } from '@tanstack/react-query';
import { useMutationWithMessage, useSaveMutation } from '@/hooks/useMutation';
import { createQueryKeys } from '@/utils/query';
import type { PaginatedResult } from '@/utils/pagination';
import * as alertApi from './alert.api';
import type { Alert } from './alert.types';

export { alertApi };

export const alertKeys = {
    ...createQueryKeys('alerts'),
    rules: (params?: Record<string, unknown>) => ['alerts', 'rules', params] as const,
    records: (params?: Record<string, unknown>) => ['alerts', 'records', params] as const,
    templates: (params?: Record<string, unknown>) => ['alerts', 'templates', params] as const,
    stats: () => ['alerts', 'stats'] as const,
};

export function useAlertRuleList(params?: Record<string, unknown>) {
    return useSnapshotQuery({
        queryKey: alertKeys.rules(params),
        queryFn: () => alertApi.getRules(params),
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
    return useSnapshotQuery({
        queryKey: alertKeys.templates(params),
        queryFn: () => alertApi.getTemplates(params),
    });
}

export function useAlertStats(
    options?: Omit<UseQueryOptions<Alert.ActiveStats>, 'queryKey' | 'queryFn'>
) {
    return useSnapshotQuery({
        queryKey: alertKeys.stats(),
        queryFn: alertApi.getStats,
        ...options,
    });
}

export function useAlertRuleSave() {
    return useSaveMutation<Alert.RuleDto & { id?: string }, Alert.RuleDto, Alert.RuleDto>({
        createFn: alertApi.createRule,
        updateFn: alertApi.updateRule,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [alertKeys.all],
    });
}

export function useAlertRuleDelete() {
    return useMutationWithMessage({
        mutationFn: alertApi.deleteRule,
        successMessage: '删除成功',
        invalidateKeys: [alertKeys.all],
    });
}

export function useAlertRuleBatchDelete() {
    return useMutationWithMessage({
        mutationFn: alertApi.batchDeleteRules,
        successMessage: '批量删除成功',
        invalidateKeys: [alertKeys.all],
    });
}

export function useAlertTemplateSave() {
    return useSaveMutation<
        Alert.TemplateDto & { id?: string },
        Alert.TemplateDto,
        Alert.TemplateDto
    >({
        createFn: alertApi.createTemplate,
        updateFn: alertApi.updateTemplate,
        toUpdatePayload: ({ id: _id, ...data }) => data,
        createMessage: '创建成功',
        updateMessage: '更新成功',
        invalidateKeys: [alertKeys.all],
    });
}

export function useAlertTemplateDelete() {
    return useMutationWithMessage({
        mutationFn: alertApi.deleteTemplate,
        successMessage: '删除成功',
        invalidateKeys: [alertKeys.all],
    });
}

export function useAlertApplyTemplate() {
    return useMutationWithMessage({
        mutationFn: alertApi.applyTemplate,
        successMessage: (result) => `应用成功，已创建 ${result.success} 条规则`,
        invalidateKeys: [alertKeys.all],
    });
}

export function useAlertAcknowledge() {
    return useMutationWithMessage({
        mutationFn: alertApi.acknowledgeRecord,
        successMessage: '确认成功',
        invalidateKeys: [alertKeys.all],
    });
}

export function useAlertBatchAcknowledge() {
    return useMutationWithMessage({
        mutationFn: alertApi.batchAcknowledge,
        successMessage: '批量确认成功',
        invalidateKeys: [alertKeys.all],
    });
}

export function useDeviceOptions(options?: { enabled?: boolean }) {
    return useSnapshotQuery({
        queryKey: ['devices', 'options'],
        queryFn: alertApi.getDeviceOptions,
        enabled: options?.enabled ?? true,
    });
}
