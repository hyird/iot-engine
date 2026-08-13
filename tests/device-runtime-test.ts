import { describe, expect, test } from 'bun:test';
import { isDeviceOnline } from '../web/pages/iot/device/device.runtime';
import type { Device } from '../web/pages/iot/device/device.types';

const device = (values: Partial<Device.RealTimeData>): Device.RealTimeData =>
    ({
        id: 'device-1',
        name: '测试设备',
        device_code: 'TEST-1',
        link_id: 'link-1',
        protocol_config_id: 'protocol-1',
        status: 'enabled',
        ...values,
    }) as Device.RealTimeData;

describe('device online state', () => {
    const now = Date.parse('2026-08-13T12:00:00.000Z');

    test('uses the latest valid data time', () => {
        expect(
            isDeviceOnline(
                device({ reportTime: '2026-08-13T11:59:31.000Z', online_timeout: 30 }),
                now
            )
        ).toBeTrue();
    });

    test('does not treat a connected TCP link as online without data', () => {
        expect(
            isDeviceOnline(device({ connected: true, connectionState: 'connected' }), now)
        ).toBeFalse();
    });

    test('does not treat an edge connection as online after data becomes stale', () => {
        expect(
            isDeviceOnline(
                device({
                    connected: true,
                    edge_node_id: 'edge-1',
                    edge_transport: 'tcp',
                    edgeStatus: { state: 'connected' },
                    reportTime: '2026-08-13T11:54:59.000Z',
                    online_timeout: 300,
                }),
                now
            )
        ).toBeFalse();
    });
});
