import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { App } from 'antd';
import { useMutationWithMessage } from '@/hooks/useMutation';
import {
    configureNetwork,
    configurePlatform,
    flashFirmware,
    getEdgeDetail,
    getEdgeList,
    getFirmwareList,
    removePlatform,
    setEnrollment,
    uploadFirmware,
} from './edge.api';
import { type Edge, edgeQueryKeys } from './edge.types';

export const useEdgeList = (query?: Edge.Query, enabled = true) =>
    useQuery({ queryKey: edgeQueryKeys.list(query), queryFn: () => getEdgeList(query), enabled });

export const useEdgeDetail = (id?: string) =>
    useQuery({
        queryKey: edgeQueryKeys.detail(id),
        queryFn: () => getEdgeDetail(id as string),
        enabled: Boolean(id),
        refetchInterval: 10_000,
    });

export const useFirmwareList = (enabled = true) =>
    useQuery({ queryKey: edgeQueryKeys.firmware, queryFn: getFirmwareList, enabled });

export function useEnrollmentMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; status: 'approved' | 'rejected'; name?: string }) =>
            setEnrollment(value.id, value.status, value.name),
        successMessage: '注册状态已更新',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function useNetworkMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.NetworkDto }) =>
            configureNetwork(value.id, value.data),
        successMessage: 'br-lan 配置已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function usePlatformMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.PlatformDto }) =>
            configurePlatform(value.id, value.data),
        successMessage: '平台配置已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function usePlatformDeleteMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; platformId: string }) =>
            removePlatform(value.id, value.platformId),
        successMessage: '平台删除配置已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}

export function useFirmwareUploadMutation() {
    const queryClient = useQueryClient();
    const { message } = App.useApp();
    return useMutation({
        mutationFn: uploadFirmware,
        onSuccess: () => {
            message.success('固件上传成功');
            queryClient.invalidateQueries({ queryKey: edgeQueryKeys.firmware });
        },
    });
}

export function useFirmwareFlashMutation() {
    return useMutationWithMessage({
        mutationFn: (value: { id: string; data: Edge.FirmwareTaskDto }) =>
            flashFirmware(value.id, value.data),
        successMessage: '固件刷写任务已下发',
        invalidateKeys: [edgeQueryKeys.all],
    });
}
