import { describe, expect, test } from 'bun:test';
import {
    normalizeReportedNetwork,
    physicalNetworkInterfaces,
} from '../web/pages/iot/edge-node/edge-node.network';
import type { Edge } from '../web/pages/iot/edge-node/edge-node.types';

const interfaces: Edge.NetworkInterface[] = [
    {
        name: 'br-lan',
        displayName: '',
        mac: '34:46:63:d3:d8:55',
        up: true,
        bridge: true,
        ipv4: '192.168.1.1',
        prefixLength: 24,
        gateway: '',
        bridgePorts: ['eth0.1'],
    },
    {
        name: 'eth0.1',
        displayName: 'eth0.1',
        mac: '34:46:63:d3:d8:55',
        up: true,
        bridge: false,
        ipv4: '',
        prefixLength: 0,
        gateway: '',
        bridgePorts: [],
    },
    {
        name: 'wg',
        displayName: 'wg',
        mac: '00:00:00:00:00:00',
        up: true,
        bridge: false,
        ipv4: '100.96.0.2',
        prefixLength: 32,
        gateway: '',
        bridgePorts: [],
    },
];

describe('edge network view', () => {
    test('excludes logical bridges and platform VPN interfaces from physical interfaces', () => {
        expect(physicalNetworkInterfaces(interfaces).map((item) => item.name)).toEqual(['eth0.1']);
    });

    test('resolves a logical bridge device to its physical members', () => {
        const network: Edge.Network = {
            name: 'lan',
            mode: 'static',
            device: 'br-lan',
            up: true,
            bridge: false,
            bridgePorts: [],
            ipv4: '192.168.1.1',
            prefixLength: 24,
            gateway: '',
        };

        expect(normalizeReportedNetwork(network, interfaces)).toEqual({
            bridge: true,
            bridgePorts: ['eth0.1'],
            device: '',
        });
    });
});
