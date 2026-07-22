import {
    ApartmentOutlined,
    AppstoreOutlined,
    ClusterOutlined,
    DownOutlined,
    HddOutlined,
    LinkOutlined,
    LogoutOutlined,
    MenuFoldOutlined,
    MenuUnfoldOutlined,
    SafetyCertificateOutlined,
    SettingOutlined,
    UserOutlined,
} from '@ant-design/icons';
import {
    Avatar,
    Breadcrumb,
    Button,
    Dropdown,
    Layout,
    Menu,
    type MenuProps,
    Space,
    Spin,
} from 'antd';
import { useMemo, useState } from 'react';
import { Outlet, useLocation, useNavigate } from 'react-router-dom';
import { APP_NAME } from '@/config/app';
import { usePermissions } from '@/hooks/usePermission';
import { useCurrentUser, useLogout } from '@/pages/login';

const { Header, Sider, Content } = Layout;

const breadcrumbGroups = [
    {
        title: '设备接入',
        icon: <LinkOutlined />,
        routes: [{ path: '/iot/link', title: '链路管理', permission: 'iot:link:query' }],
    },
    {
        title: '协议管理',
        icon: <ClusterOutlined />,
        routes: [
            { path: '/iot/sl651', title: 'SL651 配置', permission: 'iot:protocol:query' },
            { path: '/iot/modbus', title: 'Modbus 配置', permission: 'iot:protocol:query' },
            { path: '/iot/s7', title: 'S7 配置', permission: 'iot:protocol:query' },
        ],
    },
    {
        title: '设备运营',
        icon: <HddOutlined />,
        routes: [{ path: '/device', title: '设备管理', permission: 'iot:device:query' }],
    },
    {
        title: '系统管理',
        icon: <SettingOutlined />,
        routes: [
            { path: '/system/role', title: '角色管理', permission: 'system:role:query' },
            { path: '/system/dept', title: '部门管理', permission: 'system:dept:query' },
            { path: '/system/user', title: '用户管理', permission: 'system:user:query' },
        ],
    },
];

export default function AdminLayout() {
    const [collapsed, setCollapsed] = useState(false);
    const navigate = useNavigate();
    const location = useLocation();
    const { data: user, isLoading } = useCurrentUser();
    const logout = useLogout();
    const { has } = usePermissions();
    const currentBreadcrumbGroup = breadcrumbGroups.find((group) =>
        group.routes.some((route) => route.path === location.pathname)
    );
    const currentBreadcrumbRoute = currentBreadcrumbGroup?.routes.find(
        (route) => route.path === location.pathname
    );
    const siblingRoutes =
        currentBreadcrumbGroup?.routes.filter((route) => has(route.permission)) ?? [];

    const breadcrumbItems = currentBreadcrumbGroup
        ? [
              {
                  title:
                      siblingRoutes.length > 1 ? (
                          <Dropdown
                              trigger={['hover']}
                              menu={{
                                  items: siblingRoutes.map((route) => ({
                                      key: route.path,
                                      label: route.title,
                                      disabled: route.path === location.pathname,
                                      onClick: () => navigate(route.path),
                                  })),
                              }}
                          >
                              <span className="inline-flex cursor-pointer items-center gap-1 hover:text-blue-600">
                                  {currentBreadcrumbGroup.icon}
                                  {currentBreadcrumbGroup.title}
                                  <DownOutlined className="text-[10px]" />
                              </span>
                          </Dropdown>
                      ) : (
                          <span className="inline-flex items-center gap-1">
                              {currentBreadcrumbGroup.icon}
                              {currentBreadcrumbGroup.title}
                          </span>
                      ),
              },
              {
                  title: (
                      <span className="inline-flex items-center gap-1 font-medium text-slate-700">
                          {currentBreadcrumbRoute?.title}
                      </span>
                  ),
              },
          ]
        : [
              {
                  title: (
                      <span className="inline-flex items-center gap-1">
                          <AppstoreOutlined />
                          运营控制台
                      </span>
                  ),
              },
          ];

    const menuItems = useMemo(() => {
        const items: MenuProps['items'] = [];
        if (has('iot:link:query')) {
            items.push({
                key: '/iot/link',
                icon: <LinkOutlined />,
                label: '链路管理',
            });
        }
        if (has('iot:protocol:query')) {
            items.push({
                key: '/iot/protocol',
                icon: <ClusterOutlined />,
                label: '协议管理',
                children: [
                    { key: '/iot/sl651', label: 'SL651 配置' },
                    { key: '/iot/modbus', label: 'Modbus 配置' },
                    { key: '/iot/s7', label: 'S7 配置' },
                ],
            });
        }
        if (has('iot:device:query')) {
            items.push({ key: '/device', icon: <HddOutlined />, label: '设备管理' });
        }
        const systemItems: NonNullable<MenuProps['items']> = [];
        if (has('system:role:query')) {
            systemItems.push({
                key: '/system/role',
                icon: <SafetyCertificateOutlined />,
                label: '角色管理',
            });
        }
        if (has('system:dept:query')) {
            systemItems.push({
                key: '/system/dept',
                icon: <ApartmentOutlined />,
                label: '部门管理',
            });
        }
        if (has('system:user:query')) {
            systemItems.push({ key: '/system/user', icon: <UserOutlined />, label: '用户管理' });
        }
        if (systemItems.length) {
            items.push({
                key: '/system',
                icon: <SettingOutlined />,
                label: '系统管理',
                children: systemItems,
            });
        }
        return items;
    }, [has]);

    if (isLoading && !user) {
        return (
            <div className="flex h-screen items-center justify-center">
                <Spin size="large" />
            </div>
        );
    }

    return (
        <Layout className="h-screen overflow-hidden bg-slate-100">
            <Sider
                width={236}
                collapsedWidth={72}
                collapsible
                collapsed={collapsed}
                trigger={null}
                breakpoint="lg"
                onBreakpoint={(broken) => setCollapsed(broken)}
                className="border-r border-white/10"
            >
                <div className="flex h-full flex-col overflow-hidden">
                    <div
                        className={`flex h-[60px] shrink-0 items-center border-b border-white/10 ${
                            collapsed ? 'justify-center px-2' : 'px-5'
                        }`}
                    >
                        <div className="flex size-9 shrink-0 items-center justify-center rounded-md bg-blue-500 text-lg text-white shadow-lg shadow-blue-950/30">
                            <AppstoreOutlined />
                        </div>
                        {!collapsed && (
                            <div className="ml-3 min-w-0">
                                <div className="truncate text-[15px] font-semibold tracking-wide text-white">
                                    {APP_NAME}
                                </div>
                                <div className="mt-0.5 text-[10px] tracking-[0.16em] text-slate-400">
                                    INDUSTRIAL IOT
                                </div>
                            </div>
                        )}
                    </div>
                    {!collapsed && (
                        <div className="px-5 pb-2 pt-5 text-[11px] font-medium tracking-[0.14em] text-slate-500">
                            运营控制台
                        </div>
                    )}
                    <Menu
                        theme="dark"
                        mode="inline"
                        selectedKeys={[location.pathname]}
                        defaultOpenKeys={['/iot/protocol', '/system']}
                        items={menuItems}
                        onClick={({ key }) => navigate(key)}
                        className="min-h-0 flex-1 overflow-y-auto border-none py-1"
                    />
                    {!collapsed && (
                        <div className="m-4 rounded-md border border-white/10 bg-white/5 px-3 py-3">
                            <div className="text-xs font-medium text-slate-300">
                                工业物联管理中台
                            </div>
                            <div className="mt-1 text-[11px] leading-4 text-slate-500">
                                设备、协议与组织权限统一管理
                            </div>
                        </div>
                    )}
                </div>
            </Sider>
            <Layout className="min-w-0">
                <Header className="z-10 flex h-[60px] shrink-0 items-center justify-between border-b border-slate-200 bg-white px-3 sm:px-5">
                    <div className="flex min-w-0 flex-1 items-center gap-2">
                        <Button
                            type="text"
                            aria-label={collapsed ? '展开侧边栏' : '收起侧边栏'}
                            icon={collapsed ? <MenuUnfoldOutlined /> : <MenuFoldOutlined />}
                            onClick={() => setCollapsed((value) => !value)}
                        />
                        <div className="hidden min-w-0 sm:block">
                            <Breadcrumb items={breadcrumbItems} />
                        </div>
                    </div>
                    <Dropdown
                        trigger={['click']}
                        menu={{
                            items: [
                                {
                                    key: 'logout',
                                    icon: <LogoutOutlined />,
                                    label: '退出登录',
                                },
                            ],
                            onClick: () => logout.mutate(),
                        }}
                    >
                        <Button type="text" className="h-10 px-2">
                            <Space size={10}>
                                <Avatar
                                    size={30}
                                    icon={<UserOutlined />}
                                    className="bg-blue-50 text-blue-700"
                                />
                                <span className="hidden text-sm font-medium text-slate-700 sm:inline">
                                    {user?.nickname || user?.username}
                                </span>
                            </Space>
                        </Button>
                    </Dropdown>
                </Header>
                <Content className="m-3 min-h-0 overflow-hidden rounded-lg bg-white shadow-sm ring-1 ring-slate-200/80 sm:m-4">
                    <Outlet />
                </Content>
            </Layout>
        </Layout>
    );
}
