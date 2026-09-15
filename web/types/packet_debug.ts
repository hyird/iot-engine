export interface DebugPacket {
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
    storage_status?: string;
    revision?: string;
    reply_to_packet_id?: string;
    reason?: string;
    history_id?: string;
    parsed_json?: string;
}
