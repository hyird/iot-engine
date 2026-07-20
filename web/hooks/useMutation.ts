/**
 * 通用 Mutation Hook
 */

import {
    type QueryKey,
    useMutation as useReactMutation,
    useQueryClient,
} from '@tanstack/react-query';
import { App } from 'antd';

interface MutationOptions<TData, TVariables> {
    mutationFn: (variables: TVariables) => Promise<TData>;
    successMessage?: string | ((data: TData, variables: TVariables) => string);
    errorMessage?: string | ((error: Error) => string);
    invalidateKeys?: QueryKey[];
    onSuccess?: (data: TData, variables: TVariables) => void;
    onError?: (error: Error, variables: TVariables) => void;
}

export function useMutationWithMessage<TData = void, TVariables = void>(
    options: MutationOptions<TData, TVariables>
) {
    const queryClient = useQueryClient();
    const { message } = App.useApp();

    return useReactMutation({
        mutationFn: options.mutationFn,
        onSuccess: (data, variables) => {
            if (options.successMessage) {
                const msg =
                    typeof options.successMessage === 'function'
                        ? options.successMessage(data, variables)
                        : options.successMessage;
                message.success(msg);
            }

            options.invalidateKeys?.forEach((key) => {
                queryClient.invalidateQueries({ queryKey: key });
            });

            options.onSuccess?.(data, variables);
        },
        onError: (error: Error, variables) => {
            if (options.errorMessage) {
                const msg =
                    typeof options.errorMessage === 'function'
                        ? options.errorMessage(error)
                        : options.errorMessage;
                message.error(msg);
            }

            options.onError?.(error, variables);
        },
    });
}

interface SaveMutationOptions<TData, TCreateData, TUpdateData> {
    createFn: (data: TCreateData) => Promise<TData> | Promise<void>;
    updateFn: (id: number, data: TUpdateData) => Promise<TData> | Promise<void>;
    toUpdatePayload: (data: TData) => TUpdateData;
    createMessage?: string;
    updateMessage?: string;
    invalidateKeys?: QueryKey[];
}

export function useSaveMutation<TData, TCreateData, TUpdateData>(
    options: SaveMutationOptions<TData, TCreateData, TUpdateData>
) {
    const queryClient = useQueryClient();
    const { message } = App.useApp();

    return useReactMutation({
        mutationFn: async (data: TData & { id?: number }): Promise<void> => {
            if (data.id) {
                await options.updateFn(data.id as number, options.toUpdatePayload(data));
            } else {
                await options.createFn(data as unknown as TCreateData);
            }
        },
        onSuccess: (_, variables) => {
            const isUpdate = !!(variables as TData & { id?: number }).id;
            message.success(isUpdate ? options.updateMessage : options.createMessage);

            options.invalidateKeys?.forEach((key) => {
                queryClient.invalidateQueries({ queryKey: key });
            });
        },
    });
}
