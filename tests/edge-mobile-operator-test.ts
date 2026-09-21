import { expect, test } from 'bun:test';
import { mobileOperatorName } from '../web/pages/iot/edge_node/edge_node.service';

test('maps China PLMN codes to short Chinese operator names', () => {
    expect(mobileOperatorName('46011')).toBe('中国电信');
    expect(mobileOperatorName('46000')).toBe('中国移动');
    expect(mobileOperatorName('46001')).toBe('中国联通');
    expect(mobileOperatorName('46015')).toBe('中国广电');
    expect(mobileOperatorName(' 46011 ')).toBe('中国电信');
});

test('maps global PLMN codes to short brand names', () => {
    expect(mobileOperatorName('310260')).toBe('T-Mobile');
    expect(mobileOperatorName('23415')).toBe('Vodafone UK');
    expect(mobileOperatorName('44010')).toBe('NTT docomo');
    expect(mobileOperatorName('50212')).toBe('Maxis');
});

test('keeps non-PLMN values and unknown codes unchanged', () => {
    expect(mobileOperatorName('中国电信')).toBe('中国电信');
    expect(mobileOperatorName('CHN-CT')).toBe('CHN-CT');
    expect(mobileOperatorName('')).toBe('');
    expect(mobileOperatorName('99998')).toBe('99998');
});
