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
import { buildPacketTree, describeProtocolPacket } from '../web/utils/packet_debug';
import type { DebugPacket } from '../web/types/packet_debug';
const packet = (hex: string, direction = 'TX', id = 'p', reply?: string): DebugPacket => ({id, acquisition_id:'round', direction, source:'collector', time_ms:'1', payload_hex:hex, reply_to_packet_id:reply});

describe('终端协议说明与采集树', () => {
    test('乱序响应按明确关联挂接，孤立、重复内容和循环报文都保留', () => {
        const packets = [packet('0103','RX','rx','tx'), packet('0103','TX','tx'), packet('0103','RX','orphan','missing'), packet('0103','TX','cycle1','cycle2'), packet('0103','RX','cycle2','cycle1')];
        const roots = buildPacketTree(packets);
        expect(roots.find(n=>n.packet.id==='tx')?.children.map(n=>n.packet.id)).toEqual(['rx']);
        expect(roots.map(n=>n.packet.id).sort()).toEqual(['cycle1','cycle2','orphan','tx']);
        expect(packets[0].id).toBe('rx');
    });
    test('状态更新继续使用原报文，不因内容相同合并不同报文', () => {
        const before = packet('09DE00000006010300000002');
        const tree = buildPacketTree([{...before,transport_status:'sending'},{...before,transport_status:'sent'},packet(before.payload_hex,'TX','second')]);
        expect(tree).toHaveLength(2);
        expect(tree.find(n=>n.packet.id==='p')?.packet.transport_status).toBe('sent');
    });
    test('Modbus 请求、响应、异常及 RTU', () => {
        expect(describeProtocolPacket('Modbus', packet('09DE00000006010300000002'))).toContain('起始地址 0 · 数量 2');
        expect(describeProtocolPacket('Modbus', packet('09DE0000000701030440400000','RX'))).toContain('数据 4 字节');
        expect(describeProtocolPacket('Modbus', packet('09DE00000003018302','RX'))).toContain('异常码 0x02');
        expect(describeProtocolPacket('Modbus', packet('010300000002C40B'))).toContain('RTU · 站号 1');
    });
    test('S7 按区域、DB、地址和长度显示', () => {
        expect(describeProtocolPacket('S7', packet('0300001F02F080320100000001000E00000401120A10020002000184000050'))).toContain('DB1 地址 10.0 · 长度 2 byte');
    });
    test('SL651 保留功能码和流水号', () => {
        expect(describeProtocolPacket('SL651', packet('7E7E010001000102FFFA320031020104260915140000F1F1000100010249F0F02609151400392300001052371B000512272B0000001181FFB028000000001603CEA9','RX'))).toContain('功能码 0x32');
        expect(describeProtocolPacket('SL651', packet('7E7E010001000102FFFA320031020104260915140000F1F1000100010249F0F02609151400392300001052371B000512272B0000001181FFB028000000001603CEA9','RX'))).toContain('流水号 260');
    });
    test('MC、FINS 和 DL/T645 使用各自字段', () => {
        expect(describeProtocolPacket('MC', packet('500000FFFF03000C00100001040000640000A80200'))).toContain('地址 100 · 数量 2');
        expect(describeProtocolPacket('FINS', packet('46494E530000001A0000000200000000800002000100000200010101820064000002'))).toContain('地址 100.0 · 数量 2');
        expect(describeProtocolPacket('DLT645', packet('6812907856341268110433333433AE16'))).toContain('数据标识 00010000');
    });
    test('不完整、无效和未知帧不抛异常', () => {
        for (const protocol of ['Modbus','S7','SL651','MC','FINS','DLT645','未知']) {
            for (const hex of ['','1','GG','00','7E7E','030000','FEFE']) expect(()=>describeProtocolPacket(protocol,packet(hex))).not.toThrow();
        }
    });
});
import {formatDebugTerminal} from '../web/utils/packet_debug';
test('纯文本终端：设备整轮一段，链路无轮次与解析结果，过滤终端控制字符', () => {
    const rounds: DebugAcquisition[] = [{id:'round',started_at_ms:'1000',last_packet_at_ms:'1001',state:'success',parsed_json:JSON.stringify({values:{level:{name:'水位',value:3,unit:'m'}}}),packets:[packet('09DE00000006010300000002','TX','tx'),{...packet('09DE0000000701030440400000','RX','rx','tx'),reason:'bad\u001b[2J\u0007'}]}];
    const device=formatDebugTerminal('device','Modbus',rounds);
    expect(device).toContain('├─ TCP');
    expect(device.match(/本轮解析结果/g)).toHaveLength(1);
    expect(device).toContain('水位 = 3 m');
    expect(device).not.toContain('\u001b');
    expect(device).not.toContain('\u0007');
    const link=formatDebugTerminal('link','Modbus',rounds);
    expect(link).toContain('↑ TX');
    expect(link).toContain('↓ RX');
    expect(link).not.toContain('本轮解析结果');
    expect(link).not.toContain('采集完成');
    expect(formatDebugTerminal('device','Modbus',[])).toBe('');
});
