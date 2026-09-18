import { describe, expect, test } from 'bun:test';
import { dtuAsciiPacketSchema, dtuChannelSchema } from '../web/pages/iot/edge_node/edge_node.schema';

const config = {
    channelId: '00000000-0000-4000-8000-000000000001', name: 'DTU', enabled: true,
    southMode: 'tcp_server', southHost: '0.0.0.0', southPort: 5000,
    northHost: '192.0.2.10', northPort: 9000, maxClients: 3, queueBytes: 4096,
    serialFrameMs: 0,
};

describe('DTU 注册包和心跳包', () => {
    test('ASCII 按原字节编码，包括换行和空字符', () => {
        expect(dtuAsciiPacketSchema.parse('DTU\r\n\0')).toBe('4454550D0A00');
        expect(dtuAsciiPacketSchema.parse('PING')).toBe('50494E47');
        expect(dtuAsciiPacketSchema.safeParse('中文').success).toBe(false);
        expect(dtuAsciiPacketSchema.safeParse('é').success).toBe(false);
        expect(dtuAsciiPacketSchema.safeParse('A'.repeat(257)).success).toBe(false);
    });
    test('注册包和心跳包分别配置，HEX 可包含全部二进制字节', () => {
        const value = dtuChannelSchema.parse({...config, registrationHex: '4445563031', heartbeatHex: '00ff', heartbeatIntervalSec: 30});
        expect(value.registrationHex).toBe('4445563031');
        expect(value.heartbeatHex).toBe('00ff');
        expect(value.heartbeatIntervalSec).toBe(30);
        expect(dtuChannelSchema.safeParse({...config, heartbeatHex: '0FF'}).success).toBe(false);
    });
    test('旧配置默认不发心跳，启用须有内容，关闭可保留内容', () => {
        expect(dtuChannelSchema.parse(config).heartbeatIntervalSec).toBeUndefined();
        expect(dtuChannelSchema.safeParse({...config, heartbeatIntervalSec: 30}).success).toBe(false);
        expect(dtuChannelSchema.safeParse({...config, heartbeatIntervalSec: 0, heartbeatHex: '50494E47'}).success).toBe(true);
        expect(dtuChannelSchema.safeParse({...config, heartbeatIntervalSec: 86401, heartbeatHex: '00'}).success).toBe(false);
    });
});
