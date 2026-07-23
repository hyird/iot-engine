export namespace Edge {
    export type EnrollmentStatus = 'pending' | 'approved' | 'rejected';

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
        applyStatus: 'pending' | 'applied' | 'failed';
        lastMessage: string;
    }

    export interface Task {
        id: string;
        taskType: 'network' | 'firmware' | 'platform_upsert' | 'platform_delete';
        status: 'pending' | 'accepted' | 'running' | 'succeeded' | 'failed';
        message: string;
        createdAt: string;
        updatedAt: string;
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
        online: boolean;
        supportsNetworkConfig: boolean;
        supportsFirmwareUpdate: boolean;
        supportsPlatformConfig: boolean;
        supportsDeviceConfig: boolean;
        ttydAvailable: boolean;
        activeConfigVersion: number;
        desiredConfigVersion: number;
        configStatus: 'idle' | 'pending' | 'applied' | 'rejected';
        configMessage: string;
        outboxRecords: number;
        outboxBytes: number;
        lastSeenAt: string;
        createdAt: string;
        interfaces?: NetworkInterface[];
        serialPorts?: SerialPort[];
        platforms?: Platform[];
        tasks?: Task[];
    }

    export interface NetworkDto {
        ip: string;
        netmask: string;
        gateway?: string;
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
        version: string;
        file: File;
        keepSettings: boolean;
    }
}

export const edgeQueryKeys = {
    all: ['edge'] as const,
    list: (query?: Edge.Query) => [...edgeQueryKeys.all, 'list', query ?? {}] as const,
    detail: (id?: string) => [...edgeQueryKeys.all, 'detail', id ?? ''] as const,
};
