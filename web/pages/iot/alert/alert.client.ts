import request from '@/utils/http';
import { appendQueryParams } from '@/utils/query';
import type { PaginatedResult } from '@/utils/types';
import type { Device } from '../device/device.types';
import type { Alert } from './alert.types';

const BASE = '/v1/alert';

export const getRules = (params?: Record<string, unknown>) =>
    request.get<PaginatedResult<Alert.RuleItem>>(appendQueryParams(`${BASE}/rules`, params));
export const getRuleDetail = (id: string) => request.get<Alert.RuleItem>(`${BASE}/rules/${id}`);
export const createRule = (data: Alert.RuleDto) => request.post<void>(`${BASE}/rules`, data);
export const updateRule = (id: string, data: Alert.RuleDto) =>
    request.put<void>(`${BASE}/rules/${id}`, data);
export const deleteRule = (id: string) => request.delete<void>(`${BASE}/rules/${id}`);
export const batchDeleteRules = (ids: string[]) =>
    request.delete<void>(`${BASE}/rules`, { data: { ids } });
export const applyTemplate = (data: Alert.ApplyTemplateRequest) =>
    request.post<Alert.ApplyTemplateResponse>(`${BASE}/rules/apply-template`, data);

export const getTemplates = (params?: Record<string, unknown>) =>
    request.get<PaginatedResult<Alert.TemplateItem>>(
        appendQueryParams(`${BASE}/templates`, params)
    );
export const getTemplateDetail = (id: string) =>
    request.get<Alert.TemplateDetail>(`${BASE}/templates/${id}`);
export const createTemplate = (data: Alert.TemplateDto) =>
    request.post<void>(`${BASE}/templates`, data);
export const updateTemplate = (id: string, data: Alert.TemplateDto) =>
    request.put<void>(`${BASE}/templates/${id}`, data);
export const deleteTemplate = (id: string) => request.delete<void>(`${BASE}/templates/${id}`);

export const getRecords = (params?: Record<string, unknown>) =>
    request.get<PaginatedResult<Alert.RecordItem>>(appendQueryParams(`${BASE}/records`, params));
export const acknowledgeRecord = (id: string) => request.post<void>(`${BASE}/records/${id}/ack`);
export const batchAcknowledge = (ids: string[]) =>
    request.post<void>(`${BASE}/records/batch-ack`, { ids });
export const getStats = () => request.get<Alert.ActiveStats>(`${BASE}/stats`);
export const getGrouped = (days = 7) =>
    request.get<Alert.GroupedRecord[]>(appendQueryParams(`${BASE}/records/grouped`, { days }));

export const getDeviceOptions = () => request.get<Device.Option[]>('/v1/device/options');
