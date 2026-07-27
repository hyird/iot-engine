export namespace GB28181 {
    export interface Health {
        status: string;
        service: string;
        enabled: boolean;
        started: boolean;
        error: string;
    }

    export interface SipConfig {
        domain: string;
        id: string;
        host: string;
        public_ip: string;
        port: number;
        transport: string;
    }

    export interface Channel {
        id: string;
        name: string;
        manufacturer: string;
        online: boolean;
        ptz_type: number;
        ptz_capable: boolean;
    }

    export interface RecordItem {
        device_id: string;
        name: string;
        file_path: string;
        address: string;
        start_time: string;
        end_time: string;
        type: string;
        recorder_id: string;
    }

    export interface Device {
        id: string;
        name: string;
        manufacturer: string;
        remote_address: string;
        remote_ip: string;
        remote_port: string;
        registration_source: string;
        online: boolean;
        channels: Channel[];
        records: RecordItem[];
    }

    export interface StreamStatus {
        app: string;
        stream: string;
        schema: string;
        online: boolean;
        reader_count: number;
    }

    export interface Items<T> {
        items: T[];
    }

    export interface PlayUrls {
        http_flv: string;
        ws_flv: string;
        http_ts: string;
        hls: string;
        webrtc: string;
        rtsp: string;
    }

    export interface PreviewStartResult {
        sent: boolean;
        session_id: string;
        device_id: string;
        channel_id: string;
        stream_id: string;
        ssrc: string;
        rtp_port: number;
        play_urls: PlayUrls;
    }

    export interface PreviewStopResult {
        stopped: boolean;
        session_id: string;
        stream_id: string;
        bye_sent: boolean;
        rtp_server_closed: boolean;
    }

    export interface CommandResult {
        sent: boolean;
        device_id?: string;
        channel_id?: string;
        action?: string;
        speed?: number;
    }

    export interface StartPreviewPayload {
        deviceId: string;
        channelId: string;
    }

    export interface StopPreviewPayload {
        sessionId: string;
    }

    export interface PtzPayload {
        deviceId: string;
        channelId: string;
        action: PtzAction;
        speed: number;
    }

    export interface PtzPositionPayload {
        deviceId: string;
        channelId: string;
        pan: number;
        tilt: number;
        zoom: number;
    }

    export interface RecordQueryPayload {
        deviceId: string;
        channelId: string;
        startTime: string;
        endTime: string;
    }

    export type PtzAction = 'left' | 'right' | 'up' | 'down' | 'zoomin' | 'zoomout' | 'stop';
}
