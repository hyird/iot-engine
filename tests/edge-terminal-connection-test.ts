import { expect, test } from 'bun:test';
import { openTerminalSocket } from '../web/pages/iot/edge_node/edge_node.service';

test('terminal connection preserves ticket encoding and binary frames through its service interface', async () => {
    const previousWindow = Object.getOwnPropertyDescriptor(globalThis, 'window');
    const ticket = 'one-use/?&=+% ticket';
    let receivedPath = '';
    let receivedTicket: string | null = null;
    const server = Bun.serve({
        hostname: '127.0.0.1',
        port: 0,
        fetch(request, server) {
            const url = new URL(request.url);
            receivedPath = url.pathname;
            receivedTicket = url.searchParams.get('ticket');
            if (server.upgrade(request)) return;
            return new Response('upgrade required', { status: 400 });
        },
        websocket: {
            open(socket) { socket.send(new Uint8Array([0x1a, 0x04, 0x08, 0x78, 0x10, 0x1e])); },
            message() {},
        },
    });
    let socket: WebSocket | undefined;
    try {
        Object.defineProperty(globalThis, 'window', { configurable: true, value: {
            location: { protocol: 'http:', host: `127.0.0.1:${server.port}` },
        } });
        socket = openTerminalSocket(ticket);
        expect(socket.binaryType).toBe('arraybuffer');
        const frame = await new Promise<ArrayBuffer>((resolve, reject) => {
            socket!.onmessage = (event) => resolve(event.data);
            socket!.onerror = () => reject(new Error('terminal connection failed'));
        });
        expect(frame).toBeInstanceOf(ArrayBuffer);
        expect([...new Uint8Array(frame)]).toEqual([0x1a, 0x04, 0x08, 0x78, 0x10, 0x1e]);
        expect(receivedPath).toBe('/edge/v1/terminal');
        expect(receivedTicket).toBe(ticket);
    } finally {
        socket?.close();
        server.stop(true);
        if (previousWindow) Object.defineProperty(globalThis, 'window', previousWindow);
        else Reflect.deleteProperty(globalThis, 'window');
    }
});
