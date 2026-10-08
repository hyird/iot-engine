declare module 'zlmrtc-client' {
    export const Events: {
        WEBRTC_ON_REMOTE_STREAMS: string;
        WEBRTC_NOT_SUPPORT: string;
        WEBRTC_ICE_CANDIDATE_ERROR: string;
        WEBRTC_OFFER_ANSWER_EXCHANGE_FAILED: string;
        WEBRTC_ON_CONNECTION_STATE_CHANGE: string;
    };

    export class Endpoint {
        constructor(options: Record<string, unknown>);
        pc?: RTCPeerConnection | null;
        on(event: string, listener: (...args: unknown[]) => void): void;
        close(): void;
    }
}
