import type { Edge } from './edge-node.types';

export namespace EdgeVpn {
    export type NetworkStatus = 'enabled' | 'disabled';
    export type PeerStatus = 'pending' | 'active' | 'revoked';
    export type RouteStatus = 'active' | 'error' | 'disabled';
    export type RouteMode = 'nat' | 'routed';

    export interface Network {
        id: string;
        name: string;
        overlayCidr: string;
        hubPublicKey: string;
        hubEndpoint: string;
        hubListenPort: number;
        status: NetworkStatus;
        peerCount: number;
        routeCount: number;
    }

    export interface Peer {
        id: string;
        networkId: string;
        peerType: 'edge' | 'windows';
        edgeNodeId?: string;
        name: string;
        publicKey: string;
        assignedIpv4: string;
        allowedRoutes: string[];
        status: PeerStatus;
        configRevision: number;
        lastHandshakeAt?: string | null;
    }

    export interface Route {
        id: string;
        networkId: string;
        edgePeerId: string;
        edgeNodeId: string;
        lanInterface: string;
        targetCidr: string;
        virtualCidr: string;
        mode: RouteMode;
        natMode: 'masquerade' | 'none';
        status: RouteStatus;
        enabled: boolean;
        lastError: string;
    }

    export interface PeerCreateDto {
        networkId?: string;
        peerType: 'edge';
        edgeNodeId: string;
        name: string;
    }

    export interface NetworkCreateDto {
        name: string;
        overlayCidr?: string;
        hubEndpoint?: string;
        hubListenPort?: number;
    }

    export interface RouteDto {
        networkId: string;
        edgePeerId: string;
        lanInterface: string;
        targetCidr: string;
        virtualCidr: string;
        mode: RouteMode;
        enabled: boolean;
    }

    export interface ClientConfigCreateDto {
        name: string;
    }

    export interface ClientConfig {
        peerId: string;
        name: string;
        assignedIpv4: string;
        allowedRoutes: string[];
        config: string;
    }

    export interface Data {
        networks: Network[];
        peers: Peer[];
        routes: Route[];
    }

    export type Node = Edge.Node;
}

export const edgeVpnQueryKeys = {
    all: ['edge-vpn'] as const,
    node: (nodeId?: string) => [...edgeVpnQueryKeys.all, nodeId ?? ''] as const,
};
