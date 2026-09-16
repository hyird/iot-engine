export interface DebugPacket {
    acquisition_id: string;
    id: string;
    device_id?: string;
    direction: string;
    source: string;
    address?: string;
    payload_hex: string;
    time_ms: string;
    transport_status?: string;
    response_status?: string;
    parse_status?: string;
    revision?: string;
    reply_to_packet_id?: string;
    reason?: string;
    parsed_json?: string;
}

export interface DebugAcquisition {
    id: string;
    started_at_ms: string;
    finished_at_ms?: string;
    last_packet_at_ms: string;
    state: 'running' | 'success' | 'partial' | 'failed' | 'unreported';
    device_id?: string;
    packets: DebugPacket[];
}
