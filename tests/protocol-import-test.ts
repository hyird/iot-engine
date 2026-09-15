import { expect, test } from 'bun:test';
import { parseProtocolImport } from '../web/pages/iot/protocol/protocol.schema';

const item = {
    name: '导入协议', protocol: 'SL651',
    config: { responseMode: 'M3', storagePolicy: 'report', funcs: [] },
};

test('protocol import validates every item and keeps input order', () => {
    const result = parseProtocolImport(JSON.stringify([item, { ...item, name: '第二项' }]), 'SL651');
    expect(result.map(value => value.name)).toEqual(['导入协议', '第二项']);
    expect(() => parseProtocolImport(JSON.stringify([item, null]), 'SL651')).toThrow('第 2 项必须是对象');
    expect(() => parseProtocolImport(JSON.stringify([item, { ...item, name: '' }]), 'SL651')).toThrow('第 2 项 name：');
});

test('protocol import rejects malformed files and mismatched protocols with existing messages', () => {
    expect(() => parseProtocolImport('{', 'SL651')).toThrow('JSON 格式错误');
    for (const input of ['[]', '{}', 'null', 'false']) {
        expect(() => parseProtocolImport(input, 'SL651')).toThrow('文件内容为空或格式不正确');
    }
    expect(() => parseProtocolImport(JSON.stringify([item]), 'S7')).toThrow('第 1 项协议类型为 SL651，不能导入到 S7 页面');
    expect(() => parseProtocolImport('[{}]', 'SL651')).toThrow('第 1 项协议类型为 未指定，不能导入到 SL651 页面');
});
