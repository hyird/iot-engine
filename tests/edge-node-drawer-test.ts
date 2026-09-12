import { expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';

const source = readFileSync(
    new URL('../web/pages/iot/edge-node/index.tsx', import.meta.url),
    'utf8'
);
const deviceSource = readFileSync(
    new URL('../web/pages/iot/device/index.tsx', import.meta.url),
    'utf8'
);
const client = readFileSync(
    new URL('../web/pages/iot/edge-node/edge-node.api.ts', import.meta.url),
    'utf8'
);
const service = readFileSync(
    new URL('../web/pages/iot/edge-node/edge-node.service.ts', import.meta.url),
    'utf8'
);
const groupPanel = readFileSync(
    new URL('../web/pages/iot/edge-node/EdgeNodeGroupPanel.tsx', import.meta.url),
    'utf8'
);
const edgeProjector = readFileSync(
    new URL('../service/features/edge/edge.service.h', import.meta.url),
    'utf8'
);
const vpnDomain = readFileSync(
    new URL('../service/modules/vpn/vpn.service.h', import.meta.url),
    'utf8'
);
const vpnBackend = readFileSync(
    new URL('../service/features/vpn/vpn.service.h', import.meta.url),
    'utf8'
);
const vpnRuntime = readFileSync(
    new URL('../service/features/vpn/vpn.runtime.h', import.meta.url),
    'utf8'
);
const vpnFirewall = readFileSync(
    new URL('../service/features/vpn/firewall/firewall.transport.h', import.meta.url),
    'utf8'
);
const cardStart = source.indexOf('{items.map((node) => {');
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

test('dense device cards span two tracks while every card stretches to its row height', () => {
    expect(deviceSource).toContain('WIDE_DEVICE_CARD_ITEM_COUNT');
    expect(deviceSource).toContain("wide ? 'lg:col-span-2' : ''");
    expect(deviceSource).toContain('flex h-full min-w-0 flex-col');
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

test('edge nodes use hierarchical groups without repeating the group inside cards', () => {
    expect(source).toContain('<EdgeNodeGroupPanel');
    expect(source).toContain('设置分组');
    expect(source).toContain("label: 'VPN 虚拟网段'");
    expect(source).not.toContain("{ key: 'group', label: '分组'");
    expect(groupPanel).toContain('全部节点');
    expect(groupPanel).toContain('未分组');
    expect(groupPanel).toContain('新增子分组');
    expect(client).toContain('`${BASE}/groups`');
    expect(client).toContain('`${BASE}/${edgeIdSchema.parse(id)}/group`');
});

test('edge bridge routes commit before configuration is queued', () => {
    const sync = edgeProjector.indexOf('co_await service::vpn::feature::syncEdgeBridgeRoutes');
    const commit = edgeProjector.indexOf('co_await transaction.commit()', sync);
    const queue = edgeProjector.indexOf('co_await service::vpn::queueEdgeConfig', sync);
    expect(sync).toBeGreaterThanOrEqual(0);
    expect(commit).toBeGreaterThan(sync);
    expect(queue).toBeGreaterThan(commit);
});

test('Windows VPN configurations contain only active virtual LAN routes', () => {
    expect(vpnDomain).toContain('routes.column("enabled", "r")');
    expect(vpnDomain).toContain('routes.column("status", "r")');
    expect(vpnDomain).toContain('routes.value("active")');
    expect(vpnDomain).toContain('VPN 当前没有可用虚拟网段');
    expect(vpnDomain).not.toContain('address += "/32"');
});

test('Hub firewall still isolates Windows clients from unauthorized Edge tunnels', () => {
    expect(vpnBackend).toContain('addresses.aggregate("string_agg", { addresses.column("edge_address", "access")');
    expect(vpnBackend).toContain('.from("vpn_effective_edge_access", "access")');
    expect(vpnBackend).toContain('addresses.column("peer_id", "access"), Op::kEqual, addresses.column("id", "p")');
    expect(vpnRuntime).toContain('peerRecord.edgeAddresses');
    expect(vpnRuntime).toContain('client.edgeAddresses.push_back');
    expect(vpnFirewall).toContain('client.edgeAddresses');
    expect(vpnFirewall).toContain('kOverlayPool.contains(*address)');
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

test('platform exposes mobile status without modem mutation controls', () => {
    expect(drawer).toContain('APN');
    expect(drawer).toContain('运营商');
    expect(source).not.toContain('useModemControlMutation');
    expect(client).not.toContain('/modem');
    expect(service).not.toContain('useModemControlMutation');
});
