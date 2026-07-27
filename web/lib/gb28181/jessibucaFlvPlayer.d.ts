export type JessibucaFlvPlayer = {
    close: () => void;
};

export function createJessibucaFlvPlayer(options: {
    url: string;
    canvas: HTMLCanvasElement;
    onFirstFrame?: () => void;
    onError?: (error: unknown) => void;
}): Promise<JessibucaFlvPlayer>;
