import {describe, expect, test} from 'bun:test';
import {flattenAcquisitionPackets, summarizeAcquisitions} from '../web/utils/packet_debug';
import type {DebugAcquisition} from '../web/types/packet_debug';

describe('采集轮次展示', () => {
    test('时间重叠、内容相同的轮次仍保持独立，空失败轮次也保留', () => {
        const rounds: DebugAcquisition[] = [
            {id:'a',started_at_ms:'1000',last_packet_at_ms:'1030',state:'running',packets:[
                {id:'a-rx',acquisition_id:'a',direction:'RX',source:'edge',payload_hex:'0103',time_ms:'1030',reply_to_packet_id:'a-tx'},
                {id:'a-tx',acquisition_id:'a',direction:'TX',source:'edge',payload_hex:'0103',time_ms:'1000'}]},
            {id:'b',started_at_ms:'1001',last_packet_at_ms:'1020',finished_at_ms:'1031',state:'success',history_id:'b',packets:[
                {id:'b-rx',acquisition_id:'b',direction:'RX',source:'collector',payload_hex:'0103',time_ms:'1020'}]},
            {id:'empty',started_at_ms:'1002',last_packet_at_ms:'1002',finished_at_ms:'1102',state:'failed',packets:[]}
        ];
        const original = JSON.stringify(rounds);
        const result = summarizeAcquisitions(rounds);
        expect(result.map(round=>round.id)).toEqual(['empty','b','a']);
        expect(result[2].packets.map(packet=>packet.id)).toEqual(['a-tx','a-rx']);
        expect(result[2].sent).toBe(1);
        expect(result[2].received).toBe(1);
        expect(result[0].updatedAt-result[0].startedAt).toBe(100);
        expect(flattenAcquisitionPackets(rounds).map(packet=>packet.id)).toEqual(['a-rx','b-rx','a-tx']);
        expect(JSON.stringify(rounds)).toBe(original);
    });
});
