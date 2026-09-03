import { expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';

const source = readFileSync(
    new URL('../web/pages/iot/edge-node/index.tsx', import.meta.url),
    'utf8'
);
const client = readFileSync(
    new URL('../web/pages/iot/edge-node/edge-node.client.ts', import.meta.url),
    'utf8'
);
const service = readFileSync(
    new URL('../web/pages/iot/edge-node/edge-node.service.ts', import.meta.url),
    'utf8'
);
const vpnPanel = readFileSync(
    new URL('../web/pages/iot/edge-node/EdgeVpnPanel.tsx', import.meta.url),
    'utf8'
);
const groupPanel = readFileSync(
    new URL('../web/pages/iot/edge-node/EdgeNodeGroupPanel.tsx', import.meta.url),
    'utf8'
);
const vpnClient = readFileSync(
    new URL('../web/pages/iot/edge-node/edge-node.vpn.client.ts', import.meta.url),
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
    expect(drawer).not.toContain('重新拨号');
    expect(drawer).not.toContain('修改接入设置');
    expect(drawer).toContain("key: 'firmware'");
    expect(drawer).toContain('上传固件并刷写');
    expect(drawer).toContain("item.taskType === 'firmware'");
    expect(drawer).toContain('删除注册申请');
    expect(drawer).toContain('批准注册');
});

test('VPN follows mobile status and firmware history stays in its own tab', () => {
    const mobileTab = drawer.indexOf("key: 'mobile'");
    const vpnTab = drawer.indexOf("key: 'vpn'");
    const tasksTab = drawer.indexOf("key: 'tasks'");
    const firmwareTab = drawer.indexOf("key: 'firmware'");

    expect(mobileTab).toBeGreaterThan(-1);
    expect(vpnTab).toBeGreaterThan(mobileTab);
    expect(tasksTab).toBeGreaterThan(vpnTab);
    expect(firmwareTab).toBeGreaterThan(tasksTab);
    expect(drawer).toContain("(item) => item.taskType !== 'firmware'");
    expect(drawer).toContain("(item) => item.taskType === 'firmware'");
});

test('Windows VPN downloads one complete WireGuard config per device', () => {
    expect(source).toContain('下载 VPN 配置');
    expect(source).toContain('每台 Windows 设备必须单独生成一份配置');
    expect(source).toContain('downloadClientConfig(result)');
    expect(vpnPanel).not.toContain('生成 Windows 配置');
    expect(vpnClient).toContain('`${BASE}/client-configs`');
});

test('edge nodes use hierarchical groups and cards show VPN virtual networks', () => {
    expect(source).toContain('<EdgeNodeGroupPanel');
    expect(source).toContain('设置分组');
    expect(source).toContain("label: 'VPN 虚拟网段'");
    expect(source).toContain("label: '分组'");
    expect(groupPanel).toContain('全部节点');
    expect(groupPanel).toContain('未分组');
    expect(groupPanel).toContain('新增子分组');
    expect(client).toContain('`${BASE}/groups`');
    expect(client).toContain('`${BASE}/${edgeIdSchema.parse(id)}/group`');
});

test('drawer actions render above the detail drawer', () => {
    expect(source).toContain('const EDGE_DETAIL_DRAWER_Z_INDEX = 1000;');
    expect(source).toContain(
        'const EDGE_ACTION_MODAL_Z_INDEX = EDGE_DETAIL_DRAWER_Z_INDEX + 100;'
    );
    expect(drawer).toContain('zIndex={EDGE_DETAIL_DRAWER_Z_INDEX}');
    expect(source.match(/zIndex=\{EDGE_ACTION_MODAL_Z_INDEX\}/g)).toHaveLength(6);
    expect(source.match(/zIndex: EDGE_ACTION_MODAL_Z_INDEX/g)).toHaveLength(2);
});

test('platform exposes mobile status without modem mutation controls', () => {
    expect(drawer).toContain('APN');
    expect(drawer).toContain('运营商');
    expect(source).not.toContain('useModemControlMutation');
    expect(client).not.toContain('/modem');
    expect(service).not.toContain('useModemControlMutation');
});
