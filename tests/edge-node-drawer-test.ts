import { expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';

const source = readFileSync(
    new URL('../web/pages/iot/edge-node/index.tsx', import.meta.url),
    'utf8'
);
const cardStart = source.indexOf('{nodes.map((node) => {');
const drawerStart = source.indexOf('<Drawer', cardStart);
const drawerEnd = source.indexOf('<FormModal', drawerStart);
const card = source.slice(cardStart, drawerStart);
const drawer = source.slice(drawerStart, drawerEnd);

test('edge cards only navigate to the detail drawer', () => {
    expect(cardStart).toBeGreaterThan(-1);
    expect(drawerStart).toBeGreaterThan(cardStart);
    expect(card).toContain('onClick={() => showDetail(node)}');
    expect(card).toContain('<span>查看详情</span>');
    expect(card).not.toContain('showRename(node)');
    expect(card).not.toContain('deviceConfigSync.mutate(node.id)');
    expect(card).not.toContain('showNetworkManager(node)');
    expect(card).not.toContain('showModem(node)');
    expect(card).not.toContain('showFirmware(node)');
    expect(card).not.toContain('approveEnrollment(node)');
    expect(card).not.toContain('deleteEnrollment(node)');
});

test('edge management actions live in contextual drawer sections', () => {
    expect(drawerEnd).toBeGreaterThan(drawerStart);
    expect(drawer).toContain('修改名称');
    expect(drawer).toContain('Web 终端');
    expect(drawer).toContain('管理网络接口');
    expect(drawer).toContain("key: 'config'");
    expect(drawer).toContain('同步设备配置');
    expect(drawer).toContain("key: 'mobile'");
    expect(drawer).toContain('重新拨号');
    expect(drawer).toContain('修改接入设置');
    expect(drawer).toContain("key: 'firmware'");
    expect(drawer).toContain('上传固件并刷写');
    expect(drawer).toContain("item.taskType === 'firmware'");
    expect(drawer).toContain('删除注册申请');
    expect(drawer).toContain('批准注册');
});

test('drawer actions render above the detail drawer', () => {
    expect(source).toContain('const EDGE_DETAIL_DRAWER_Z_INDEX = 1000;');
    expect(source).toContain(
        'const EDGE_ACTION_MODAL_Z_INDEX = EDGE_DETAIL_DRAWER_Z_INDEX + 100;'
    );
    expect(drawer).toContain('zIndex={EDGE_DETAIL_DRAWER_Z_INDEX}');
    expect(source.match(/zIndex=\{EDGE_ACTION_MODAL_Z_INDEX\}/g)).toHaveLength(5);
    expect(source.match(/zIndex: EDGE_ACTION_MODAL_Z_INDEX/g)).toHaveLength(2);
});
