import type { GB28181 } from './gb28181.types';

export type PlaybackCandidate = {
    decoder?: 'native-only' | 'software-only';
    engine: 'adaptive-flv' | 'hls' | 'mpegts';
    label: string;
    mediaType?: 'flv' | 'mpegts';
    url: string;
};

export type PlaybackCapabilities = {
    hls: boolean;
    mpegts: boolean;
    mseH265: boolean;
    softwareVideo: boolean;
    webCodecs: boolean;
};

const compact = (candidates: Array<PlaybackCandidate | null>) =>
    candidates.filter((candidate): candidate is PlaybackCandidate => Boolean(candidate?.url));

export function buildPlaybackCandidates(
    urls: GB28181.PlayUrls,
    capabilities: PlaybackCapabilities
): PlaybackCandidate[] {
    const nativeFlv = capabilities.webCodecs
        ? compact([
              urls.ws_flv
                  ? {
                        decoder: 'native-only',
                        engine: 'adaptive-flv',
                        label: 'WS-FLV · WebCodecs',
                        url: urls.ws_flv,
                    }
                  : null,
              urls.http_flv
                  ? {
                        decoder: 'native-only',
                        engine: 'adaptive-flv',
                        label: 'HTTP-FLV · WebCodecs',
                        url: urls.http_flv,
                    }
                  : null,
          ])
        : [];
    const mse = capabilities.mpegts
        ? compact([
              urls.http_ts
                  ? {
                        engine: 'mpegts',
                        label: 'HTTP-TS · 原生解码',
                        mediaType: 'mpegts',
                        url: urls.http_ts,
                    }
                  : null,
              urls.ws_flv
                  ? {
                        engine: 'mpegts',
                        label: 'WS-FLV · 原生解码',
                        mediaType: 'flv',
                        url: urls.ws_flv,
                    }
                  : null,
              urls.http_flv
                  ? {
                        engine: 'mpegts',
                        label: 'HTTP-FLV · 原生解码',
                        mediaType: 'flv',
                        url: urls.http_flv,
                    }
                  : null,
          ])
        : [];
    const softwareFlv = capabilities.softwareVideo
        ? compact([
              urls.ws_flv
                  ? {
                        decoder: 'software-only',
                        engine: 'adaptive-flv',
                        label: 'WS-FLV · Worker软解',
                        url: urls.ws_flv,
                    }
                  : null,
              urls.http_flv
                  ? {
                        decoder: 'software-only',
                        engine: 'adaptive-flv',
                        label: 'HTTP-FLV · Worker软解',
                        url: urls.http_flv,
                    }
                  : null,
          ])
        : [];

    const ordered = capabilities.mseH265
        ? [...mse.slice(0, 1), ...nativeFlv, ...mse.slice(1), ...softwareFlv]
        : [...nativeFlv, ...mse, ...softwareFlv];
    if (capabilities.hls && urls.hls) {
        ordered.push({ engine: 'hls', label: 'HLS', url: urls.hls });
    }
    return ordered;
}
