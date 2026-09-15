import type { DebugAcquisition, DebugPacket } from '../types/packet_debug';

export interface AcquisitionSummary extends DebugAcquisition {
    startedAt: number;
    updatedAt: number;
    sent: number;
    received: number;
}

export function summarizeAcquisitions(
    acquisitions: readonly DebugAcquisition[]
): AcquisitionSummary[] {
    return acquisitions
        .map((acquisition) => {
            const packets = [...acquisition.packets].sort(
                (left, right) =>
                    Number(left.time_ms) - Number(right.time_ms) || left.id.localeCompare(right.id)
            );
            return {
                ...acquisition,
                packets,
                startedAt: Number(acquisition.started_at_ms),
                updatedAt: Number(acquisition.finished_at_ms ?? acquisition.last_packet_at_ms),
                sent: packets.filter(
                    (packet) => packet.direction === 'TX' || packet.direction === 'TX_ATTEMPT'
                ).length,
                received: packets.filter(
                    (packet) => packet.direction === 'RX' || packet.direction === 'RX_DROP'
                ).length,
            };
        })
        .sort((left, right) => right.startedAt - left.startedAt || left.id.localeCompare(right.id));
}

export function flattenAcquisitionPackets(
    acquisitions: readonly DebugAcquisition[]
): DebugPacket[] {
    return acquisitions
        .flatMap((acquisition) => acquisition.packets)
        .sort(
            (left, right) =>
                Number(right.time_ms) - Number(left.time_ms) || left.id.localeCompare(right.id)
        );
}
