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
        applyStatus: 'pending' | 'applied' | 'failed';
        lastMessage: string;
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
        networkConfigVersion: number;
        supportsFirmwareUpdate: boolean;
        supportsPlatformConfig: boolean;
        supportsDeviceConfig: boolean;
        supportsModemControl: boolean;
        ttydAvailable: boolean;
        modemAvailable: boolean;
        simState:
            | 'unknown'
            | 'ready'
            | 'not_inserted'
            | 'pin_required'
            | 'puk_required'
            | 'blocked';
        iccid: string;
        signalCsq: number;
        signalRssiDbm: number;
        signalPercent: number;
        mobileRegistered: boolean;
        mobileRegistrationStatus: number;
        apn: string;
        mobileOperator: string;
        mobileConnected: boolean;
        mobileIpv4: string;
        firmwareStatus: Task['status'] | '';
        firmwareProgressPercent: number;
        firmwareDownloadedBytes: number;
        firmwareTotalBytes: number;
        firmwareMessage: string;
        activeConfigVersion: number;
        desiredConfigVersion: number;
        configStatus: 'idle' | 'pending' | 'applied' | 'rejected';
        configMessage: string;
        outboxRecords: number;
        outboxBytes: number;
        lastSeenAt: string;
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
}

export const edgeQueryKeys = {
    all: ['edge'] as const,
    list: (query?: Edge.Query) => [...edgeQueryKeys.all, 'list', query ?? {}] as const,
    detail: (id?: string) => [...edgeQueryKeys.all, 'detail', id ?? ''] as const,
};
