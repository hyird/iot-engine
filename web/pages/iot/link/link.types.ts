import { createQueryKeys } from '@/utils/query';
import type { PageParams } from '@/utils/types';

const keys = createQueryKeys('links');
export const linkQueryKeys = {
    ...keys,
    list: (params?: Link.Query) => [...keys.lists(), params] as const,
};

type LinkMode = 'TCP Server' | 'TCP Client';
type LinkProtocol = 'SL651' | 'Modbus' | 'S7';
type LinkStatus = 'enabled' | 'disabled';
type ConnectionStatus =
    | 'stopped'
    | 'listening'
    | 'connected'
    | 'partial'
    | 'connecting'
    | 'reconnecting'
    | 'error';

interface LinkTarget {
    id: string;
    name: string;
    ip: string;
    port: number;
    status: LinkStatus;
    conn_status?: ConnectionStatus;
    state_reason?: string;
    error_msg?: string;
    last_activity_at_ms?: number;
}

interface LinkItem {
    id: string;
    name: string;
    mode: LinkMode;
    protocol: LinkProtocol;
    ip: string;
    port: number;
    targets: LinkTarget[];
    status: LinkStatus;
    conn_status: ConnectionStatus;
    client_count: number;
    state_reason?: string;
    clients?: string[];
    error_msg?: string;
    created_by: string;
    created_at: string;
    updated_at: string;
}

interface LinkQuery extends PageParams {
    mode?: LinkMode;
    protocol?: LinkProtocol;
    status?: LinkStatus;
}

interface SaveLinkDto {
    name: string;
    mode: LinkMode;
    protocol: LinkProtocol;
    ip: string;
    port: number;
    targets: LinkTarget[];
    status: LinkStatus;
}

interface LinkEnums {
    modes: LinkMode[];
    protocols: LinkProtocol[];
    statuses: LinkStatus[];
}

interface LinkOption extends LinkItem {}

export namespace Link {
    export type Mode = LinkMode;
    export type Protocol = LinkProtocol;
    export type Status = LinkStatus;
    export type Target = LinkTarget;
    export type Item = LinkItem;
    export type Query = LinkQuery;
    export type SaveDto = SaveLinkDto;
    export type Enums = LinkEnums;
    export type Option = LinkOption;
}
