import { expect, test } from 'bun:test';
import { mobileOperatorName } from '../web/pages/iot/edge_node/edge_node.service';

test('maps China PLMN codes to operator names and leaves other values unchanged', () => {
    expect(mobileOperatorName('46011')).toBe('中国电信');
    expect(mobileOperatorName('46000')).toBe('中国移动');
    expect(mobileOperatorName('46001')).toBe('中国联通');
    expect(mobileOperatorName('46015')).toBe('中国广电');
    expect(mobileOperatorName(' 46011 ')).toBe('中国电信');
    expect(mobileOperatorName('中国电信')).toBe('中国电信');
    expect(mobileOperatorName('CHN-CT')).toBe('CHN-CT');
    expect(mobileOperatorName('')).toBe('');
});
