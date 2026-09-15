export interface DebugPacket {
    id: string;
    device_id?: string;
    direction: string;
    source: string;
    address?: string;
    payload_hex: string;
    time_ms: string;
}
