export namespace GB28181 {
    export interface Health {
        status: string;
        service: string;
        enabled: boolean;
        started: boolean;
        error: string;
        media_ports: {
            http: number;
            https: number;
            rtsp: number;
            rtsps: number;
            rtmp: number;
            rtmps: number;
            rtc: number;
            srt: number;
        };
        media_capabilities: {
            faac: boolean;
            ffmpeg: boolean;
            hls: boolean;
            mp4: boolean;
            rtp_proxy: boolean;
            srt: boolean;
            sctp: boolean;
            web_rtc: boolean;
            x264: boolean;
            video_stack: boolean;
            tls: boolean;
            recording: boolean;
        };
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
        mapped_device_id: string;
        last_seen_at: string;
        online: boolean;
        channels: Channel[];
        records: RecordItem[];
    }

    export interface StreamStatus {
        id: string;
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
        rtmp: string;
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
        mapped_device_id?: string;
        recording?: boolean;
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

    export interface StreamPayload {
        streamId: string;
    }

    export interface MapDevicePayload {
        deviceId: string;
        mappedDeviceId: string;
    }

    export type PtzAction = 'left' | 'right' | 'up' | 'down' | 'zoomin' | 'zoomout' | 'stop';
}
