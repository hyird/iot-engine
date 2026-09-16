import dayjs from 'dayjs';
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
export interface PacketBranch {
    packet: DebugPacket;
    children: PacketBranch[];
}

/** 只按显式报文关联建树；孤立报文保留为根，错误循环不吞掉任何报文。 */
export function buildPacketTree(packets: readonly DebugPacket[]): PacketBranch[] {
    const nodes = new Map<string, PacketBranch>();
    for (const packet of [...packets].sort(
        (a, b) => Number(a.time_ms) - Number(b.time_ms) || a.id.localeCompare(b.id)
    )) {
        nodes.set(packet.id, { packet, children: [] });
    }
    const roots: PacketBranch[] = [];
    for (const node of nodes.values()) {
        const parent = nodes.get(node.packet.reply_to_packet_id ?? '');
        let cursor = parent;
        const visited = new Set([node.packet.id]);
        let cycle = false;
        while (cursor) {
            if (visited.has(cursor.packet.id)) {
                cycle = true;
                break;
            }
            visited.add(cursor.packet.id);
            cursor = nodes.get(cursor.packet.reply_to_packet_id ?? '');
        }
        if (parent && !cycle) parent.children.push(node);
        else roots.push(node);
    }
    return roots;
}

/** 展示帧中的协议字段，不代替后端校验、解码或采集状态。 */
export function describeProtocolPacket(protocol: string, packet: DebugPacket): string {
    const hex = packet.payload_hex.replace(/\s/g, '');
    if (!hex || hex.length % 2 || !/^[\da-f]+$/i.test(hex)) return '原始报文（无法读取协议字段）';
    const bytes = Array.from({ length: hex.length / 2 }, (_, i) =>
        Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16)
    );
    const h = (value: number, width = 2) => value.toString(16).toUpperCase().padStart(width, '0');
    const be16 = (i: number) => bytes[i] * 256 + bytes[i + 1];
    const le16 = (i: number) => bytes[i] + bytes[i + 1] * 256;
    const tx = packet.direction === 'TX' || packet.direction === 'TX_ATTEMPT';
    const fallback = `${protocol || '未知协议'} · ${bytes.length} 字节（非完整或未识别帧）`;
    if (protocol === 'Modbus') {
        const tcp = bytes.length >= 9 && be16(2) === 0 && be16(4) === bytes.length - 6;
        if (!tcp) {
            if (bytes.length < 5) return fallback;
            let crc = 0xffff;
            for (const byte of bytes.slice(0, -2)) {
                crc ^= byte;
                for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >>> 1) ^ 0xa001 : crc >>> 1;
            }
            if (crc !== le16(bytes.length - 2)) return fallback;
        }
        const base = tcp ? 6 : 0;
        if (bytes.length < base + 3) return fallback;
        const code = bytes[base + 1];
        const labels: Record<number, string> = {
            1: '读线圈',
            2: '读离散输入',
            3: '读保持寄存器',
            4: '读输入寄存器',
            5: '写单线圈',
            6: '写单寄存器',
            15: '写多线圈',
            16: '写多寄存器',
        };
        if (!labels[code & 0x7f]) return fallback;
        const end = bytes.length - (tcp ? 0 : 2);
        if (
            code & 0x80
                ? end !== base + 3
                : tx
                  ? end < base + 6
                  : code <= 4 && end !== base + 3 + bytes[base + 2]
        )
            return fallback;
        const parts = [
            tcp ? `TCP · 事务 ${be16(0)}` : 'RTU',
            `站号 ${bytes[base]}`,
            `功能 0x${h(code)} ${labels[code & 0x7f]}`,
        ];
        if (code & 0x80) parts.push(`异常码 0x${h(bytes[base + 2])}`);
        else if (tx && bytes.length >= base + 6) {
            parts.push(`起始地址 ${be16(base + 2)}`);
            parts.push(`${code === 5 || code === 6 ? '写入值' : '数量'} ${be16(base + 4)}`);
        } else if (!tx && code <= 4) parts.push(`数据 ${bytes[base + 2]} 字节`);
        return parts.join(' · ');
    }
    if (protocol === 'S7') {
        if (bytes.length < 7 || bytes[0] !== 3 || be16(2) !== bytes.length) return fallback;
        if (bytes[5] === 0xe0 || bytes[5] === 0xd0)
            return `S7 · COTP ${bytes[5] === 0xe0 ? '连接请求' : '连接确认'}`;
        if (bytes.length < 19 || bytes[7] !== 0x32) return fallback;
        const offset = bytes[8] === 3 ? 19 : 17;
        if (bytes.length <= offset) return fallback;
        const fn = bytes[offset];
        const parts = [
            `S7 · PDU ${be16(11)}`,
            fn === 4
                ? '读取变量'
                : fn === 5
                  ? '写入变量'
                  : fn === 0xf0
                    ? '协商 PDU'
                    : `功能 0x${h(fn)}`,
        ];
        if (tx && (fn === 4 || fn === 5) && bytes.length > offset + 1) {
            const areas: Record<number, string> = {
                132: 'DB',
                131: 'MK',
                130: 'PA',
                129: 'PE',
                28: 'CT',
                29: 'TM',
            };
            for (
                let n = 0, i = offset + 2;
                n < bytes[offset + 1] && i + 12 <= bytes.length;
                n++, i += 12
            ) {
                if (bytes[i] !== 0x12 || bytes[i + 1] !== 10) break;
                const address = bytes[i + 9] * 65536 + be16(i + 10);
                parts.push(
                    `${areas[bytes[i + 8]] ?? h(bytes[i + 8])}${bytes[i + 8] === 0x84 ? be16(i + 6) : ''} 地址 ${Math.floor(address / 8)}.${address % 8} · 长度 ${be16(i + 4)} ${bytes[i + 3] === 1 ? 'bit' : bytes[i + 3] === 2 ? 'byte' : '元素'}`
                );
            }
        } else if (bytes[8] === 3) parts.push(`错误码 0x${h(be16(17), 4)}`);
        return parts.join(' · ');
    }
    if (protocol === 'SL651') {
        if (bytes.length < 17 || be16(0) !== 0x7e7e || (be16(11) & 0xfff) + 17 !== bytes.length)
            return fallback;
        const multi = bytes[13] === 0x16;
        const start = multi ? 17 : 14;
        const parts = [`SL651 · 功能码 0x${h(bytes[10])}${bytes[10] === 0x32 ? ' 定时报' : ''}`];
        if (multi && bytes.length >= 20) {
            const packed = bytes[14] * 65536 + be16(15);
            parts.push(`分包 ${packed & 0xfff}/${packed >>> 12}`);
        }
        if (bytes.length >= start + 5) parts.push(`流水号 ${be16(start)}`);
        const end = bytes[bytes.length - 3];
        parts.push(`结束符 0x${h(end)}${end === 6 ? ' ACK' : ''}`);
        return parts.join(' · ');
    }
    if (protocol === 'MC') {
        const four = bytes[0] === 0x54 || bytes[0] === 0xd4;
        const offset = four ? 6 : 2;
        if (
            ![0x50, 0x54, 0xd0, 0xd4].includes(bytes[0]) ||
            bytes[1] !== 0 ||
            bytes.length < offset + 9 ||
            le16(offset + 5) + offset + 7 !== bytes.length
        )
            return fallback;
        const parts = [`MC · ${four ? '4E' : '3E'}`];
        if (four) parts.push(`流水号 ${le16(2)}`);
        if (bytes[0] >= 0xd0) parts.push(`结束码 0x${h(le16(offset + 7), 4)}`);
        else if (bytes.length >= offset + 19) {
            parts.push(
                `命令 0x${h(le16(offset + 9), 4)}`,
                `设备区 0x${h(bytes[offset + 16])}`,
                `地址 ${bytes[offset + 13] + bytes[offset + 14] * 256 + bytes[offset + 15] * 65536}`,
                `数量 ${le16(offset + 17)}`
            );
        }
        return parts.join(' · ');
    }
    if (protocol === 'FINS') {
        if (bytes.length < 16 || hex.slice(0, 8).toUpperCase() !== '46494E53') return fallback;
        if (be16(4) * 65536 + be16(6) + 8 !== bytes.length) return fallback;
        if (be16(8) !== 0) return fallback;
        if (be16(10) < 2) return `FINS/TCP · 节点地址${be16(10) === 0 ? '请求' : '应答'}`;
        if (bytes.length < 28) return fallback;
        const parts = [`FINS · SID ${bytes[25]}`, `命令 0x${h(be16(26), 4)}`];
        if (bytes[16] & 0x40 && bytes.length >= 30) parts.push(`结束码 0x${h(be16(28), 4)}`);
        else if (bytes.length >= 34)
            parts.push(
                `存储区 0x${h(bytes[28])}`,
                `地址 ${be16(29)}.${bytes[31]}`,
                `数量 ${be16(32)}`
            );
        return parts.join(' · ');
    }
    if (protocol === 'DLT645') {
        let start = 0;
        while (bytes[start] === 0xfe) start++;
        if (
            bytes.length < start + 12 ||
            bytes[start] !== 0x68 ||
            bytes[start + 7] !== 0x68 ||
            bytes[start + 9] + start + 12 !== bytes.length
        )
            return fallback;
        const control = bytes[start + 8];
        const parts = [
            `DL/T645 · 表地址 ${bytes
                .slice(start + 1, start + 7)
                .reverse()
                .map((b) => h(b))
                .join('')}`,
            `控制码 0x${h(control)}`,
        ];
        const fn = control & 0x1f;
        const length = fn === 0x11 || fn === 0x12 ? 4 : fn === 1 || fn === 2 ? 2 : 0;
        if (control & 0x40) parts.push('异常应答');
        else if (length && bytes[start + 9] >= length)
            parts.push(
                `数据标识 ${bytes
                    .slice(start + 10, start + 10 + length)
                    .reverse()
                    .map((b) => h((b - 0x33) & 0xff))
                    .join('')}`
            );
        return parts.join(' · ');
    }
    return `${protocol || '未知协议'} · ${bytes.length} 字节`;
}

/** 终端只接收纯文本，设备字段中的控制字符不能成为终端指令。 */
export function formatDebugTerminal(
    scope: 'device' | 'link',
    protocol: string,
    acquisitions: readonly DebugAcquisition[]
): string {
    const clean = (value: unknown) =>
        Array.from(String(value ?? ''))
            .filter((char) => {
                const code = char.charCodeAt(0);
                return code >= 32 && code !== 127 && !(code >= 128 && code <= 159);
            })
            .join('');
    const time = (value: string | number) => dayjs(Number(value)).format('YYYY-MM-DD HH:mm:ss.SSS');
    const status = (packet: DebugPacket, link: boolean) => {
        const result = [
            { sending: '发送中', sent: '已发送', received: '已接收', failed: '发送失败' }[
                packet.transport_status ?? ''
            ],
        ];
        if (!link) {
            result.push(
                { waiting: '等待应答', success: '应答成功', failed: '应答失败' }[
                    packet.response_status ?? ''
                ]
            );
            result.push(
                { pending: '待解析', success: '解析成功', failed: '解析失败' }[
                    packet.parse_status ?? ''
                ]
            );
        }
        return result.filter(Boolean).join(' · ');
    };
    const lines: string[] = [];
    const writePacket = (packet: DebugPacket, indent: string, link: boolean) => {
        const sending = packet.direction === 'TX' || packet.direction === 'TX_ATTEMPT';
        lines.push(
            `${indent}${time(packet.time_ms)}  ${sending ? '↑ TX' : '↓ RX'}  ${link ? `${clean(packet.address || '对端未知')}  ${clean(packet.device_id || '未识别设备')}  ` : ''}${status(packet, link)}`
        );
        lines.push(`${indent}${clean(packet.payload_hex)}`);
        if (packet.reason) lines.push(`${indent}原因：${clean(packet.reason)}`);
        if (link || sending) return;
        if (!packet.parsed_json) {
            lines.push(
                `${indent}解析：${packet.parse_status === 'failed' ? '解析失败' : '暂无本条应答的解析结果'}`
            );
            return;
        }
        try {
            const parsed = JSON.parse(packet.parsed_json);
            const values = parsed?.values ?? parsed;
            if (!values || typeof values !== 'object' || Array.isArray(values))
                throw new Error('invalid values');
            lines.push(`${indent}解析结果：`);
            for (const [key, point] of Object.entries(values)) {
                const item =
                    typeof point === 'object' && point !== null
                        ? (point as Record<string, unknown>)
                        : { value: point };
                const value =
                    item.value == null
                        ? '—'
                        : typeof item.value === 'object'
                          ? JSON.stringify(item.value)
                          : item.value;
                lines.push(
                    `${indent}  ${clean(item.name ?? key)} = ${clean(value)} ${clean(item.unit)}`.trimEnd()
                );
            }
            if (!Object.keys(values).length) lines.push(`${indent}  本条应答无要素值`);
        } catch {
            lines.push(`${indent}解析数据格式无效`);
        }
    };
    if (scope === 'link') {
        for (const packet of flattenAcquisitionPackets(acquisitions).reverse()) {
            writePacket(packet, '', true);
            lines.push('');
        }
    } else {
        for (const round of summarizeAcquisitions(acquisitions).reverse()) {
            const state = {
                running: '采集中',
                success: '采集完成',
                partial: '部分失败',
                failed: '采集失败',
                unreported: '未上报轮次',
            }[round.state];
            const responses = round.packets;
            if (!responses.length) continue;
            lines.push(
                `${time(round.startedAt)}  ${clean(protocol)}  ${state} · 发送 ${round.sent} / 接收 ${round.received}`
            );
            for (const response of responses) {
                lines.push(`  ├─ ${clean(describeProtocolPacket(protocol, response))}`);
                writePacket(response, '  │  ', false);
            }
            lines.push('');
        }
    }
    return lines.join('\r\n');
}
