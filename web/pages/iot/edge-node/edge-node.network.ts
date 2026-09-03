import type { Edge } from './edge-node.types';

const platformVirtualInterfaces = new Set(['lo', 'wg-iot']);

export function physicalNetworkInterfaces(interfaces: Edge.NetworkInterface[]) {
    const subinterfaceParents = new Set<string>();
    for (const item of interfaces) {
        let parent = item.name;
        while (parent.includes('.')) {
            parent = parent.slice(0, parent.lastIndexOf('.'));
            if (parent) subinterfaceParents.add(parent);
        }
    }
    return interfaces.filter(
        (item) =>
            !item.bridge &&
            !subinterfaceParents.has(item.name) &&
            !platformVirtualInterfaces.has(item.name)
    );
}

export function normalizeReportedNetwork(
    network: Edge.Network,
    interfaces: Edge.NetworkInterface[]
) {
    const reportedBridge = interfaces.find(
        (candidate) => candidate.name === network.device && candidate.bridge
    );
    const bridge = network.bridge || Boolean(reportedBridge);
    return {
        bridge,
        bridgePorts: bridge
            ? network.bridgePorts.length > 0
                ? network.bridgePorts
                : (reportedBridge?.bridgePorts ?? [])
            : [],
        device: bridge ? '' : network.device,
    };
}
