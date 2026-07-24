export namespace Edge {
    export type EnrollmentStatus = 'pending' | 'approved' | 'rejected';
    export type LogLevel = 'debug' | 'info' | 'warn' | 'error';

    export interface Query {
        page?: number;
        pageSize?: number;
        keyword?: string;
        status?: EnrollmentStatus;
    }

    export interface NetworkInterface {
        name: string;
        displayName: string;
        mac: string;
        up: boolean;
        bridge: boolean;
        ipv4: string;
        prefixLength: number;
        gateway: string;
        bridgePorts: string[];
    }

    export interface Network {
        name: string;
        mode: 'dhcp' | 'static' | 'none';
        device: string;
        up: boolean;
        bridge: boolean;
        bridgePorts: string[];
        ipv4: string;
        prefixLength: number;
        gateway: string;
    }

    export interface SerialPort {
        path: string;
        displayName: string;
        available: boolean;
        rs485: boolean;
    }

    export interface Platform {
        platformId: string;
        name: string;
        baseUrl: string;
        enabled: boolean;
        priority: number;
        reconnectIntervalSec: number;
        outboxMaxBytes: number;
        status: PlatformStatus;
    }

    export interface Task {
        id: string;
        taskType: 'network' | 'firmware' | 'modem' | 'platform_upsert' | 'platform_delete';
        status: 'pending' | 'accepted' | 'running' | 'succeeded' | 'failed';
        message: string;
        progressPercent: number;
        downloadedBytes: number;
        totalBytes: number;
        createdAt: string;
        updatedAt: string;
    }

    export interface PlatformStatus {
        state: 'pending' | 'applied' | 'failed';
        message: string;
    }

    export interface ConfigStatus {
        activeVersion: number;
        desiredVersion: number;
        state: 'idle' | 'pending' | 'applied' | 'rejected';
        message: string;
    }

    export interface OutboxStatus {
        records: number;
        bytes: number;
    }

    export interface NodeStatus {
        online: boolean;
        lastSeenAt: string;
        config: ConfigStatus;
        outbox: OutboxStatus;
    }

    export interface Capability {
        networkConfig: boolean;
        networkConfigVersion: number;
        firmwareUpdate: boolean;
        platformConfig: boolean;
        deviceConfig: boolean;
        modemControl: boolean;
        terminal: boolean;
        logs: boolean;
    }

    export interface Signal {
        csq: number;
        rssiDbm: number;
        percent: number;
    }

    export interface Mobile {
        available: boolean;
        simState:
            | 'unknown'
            | 'ready'
            | 'not_inserted'
            | 'pin_required'
            | 'puk_required'
            | 'blocked';
        iccid: string;
        signal: Signal;
        registered: boolean;
        registrationStatus: number;
        apn: string;
        operator: string;
        connected: boolean;
        ipv4: string;
    }

    export interface FirmwareStatus {
        state: Task['status'] | '';
        progressPercent: number;
        downloadedBytes: number;
        totalBytes: number;
        message: string;
    }

    export interface Node {
        id: string;
        imei: string;
        name: string;
        model: string;
        softwareVersion: string;
        hostname: string;
        architecture: string;
        openwrtRelease: string;
        enrollmentStatus: EnrollmentStatus;
        status: NodeStatus;
        capability: Capability;
        mobile: Mobile;
        firmware: FirmwareStatus;
        createdAt: string;
        interfaces?: NetworkInterface[];
        networks?: Network[];
        serialPorts?: SerialPort[];
        platforms?: Platform[];
        tasks?: Task[];
    }

    export interface NetworkConfig {
        operation: 'upsert' | 'delete';
        name: string;
        previousName?: string;
        mode?: 'dhcp' | 'static';
        device?: string;
        bridge?: boolean;
        bridgePorts?: string[];
        ip?: string;
        prefixLength?: number;
        gateway?: string;
    }

    export interface NetworkDto {
        interfaces: NetworkConfig[];
        rollbackTimeoutSec: number;
    }

    export interface NameDto {
        name: string;
    }

    export interface PlatformDto {
        platformId?: string;
        name: string;
        baseUrl: string;
        enrollmentToken?: string;
        enabled: boolean;
        priority: number;
        reconnectIntervalSec: number;
        outboxMaxBytes: number;
    }

    export interface FirmwareUpgradeDto {
        file: File;
        keepSettings: boolean;
    }

    export interface ModemControlDto {
        action: 'set_apn' | 'redial';
        apn: string;
    }

    export interface LogsQuery {
        limit?: number;
        level?: LogLevel;
        source?: string;
    }

    export interface LogLine {
        time: number;
        level: LogLevel;
        source: string;
        message: string;
        detail: string;
    }

    export interface Logs {
        lines: LogLine[];
    }
}

export const edgeQueryKeys = {
    all: ['edge'] as const,
    list: (query?: Edge.Query) => [...edgeQueryKeys.all, 'list', query ?? {}] as const,
    detail: (id?: string) => [...edgeQueryKeys.all, 'detail', id ?? ''] as const,
    logs: (id?: string, query?: Edge.LogsQuery) =>
        [...edgeQueryKeys.all, 'logs', id ?? '', query ?? {}] as const,
};
