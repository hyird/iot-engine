import { expect, test } from 'bun:test';
import { create, fromBinary, toBinary } from '@bufbuild/protobuf';
import {
    WebTerminalFrameSchema,
    WebTerminalResizeSchema,
} from '../web/generated/edge/terminal-pb';

test('web terminal protobuf matches the server golden vector', () => {
    const resize = create(WebTerminalResizeSchema, { columns: 120, rows: 30 });
    const frame = create(WebTerminalFrameSchema, {
        payload: { case: 'resize', value: resize },
    });
    const wire = toBinary(WebTerminalFrameSchema, frame);

    expect([...wire]).toEqual([0x1a, 0x04, 0x08, 0x78, 0x10, 0x1e]);
    expect(fromBinary(WebTerminalFrameSchema, wire).payload).toEqual({
        case: 'resize',
        value: resize,
    });
});
