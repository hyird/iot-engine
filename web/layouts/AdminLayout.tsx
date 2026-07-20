import {
    ApartmentOutlined,
    LogoutOutlined,
    MenuFoldOutlined,
    MenuUnfoldOutlined,
    SafetyCertificateOutlined,
    SettingOutlined,
    UserOutlined,
} from '@ant-design/icons';
import { Button, Dropdown, Layout, Menu, Space, Spin, type MenuProps } from 'antd';
import { useMemo, useState } from 'react';
import { Outlet, useLocation, useNavigate } from 'react-router-dom';
import { APP_NAME } from '@/config/app';
import { useCurrentUser, useLogout } from '@/pages/login';
import { usePermissions } from '@/hooks/usePermission';

const { Header, Sider, Content } = Layout;

export default function AdminLayout() {
    const [collapsed, setCollapsed] = useState(false);
    const navigate = useNavigate();
    const location = useLocation();
    const { data: user, isLoading } = useCurrentUser();
    const logout = useLogout();
    const { has } = usePermissions();

    const menuItems = useMemo(() => {
        const items: MenuProps['items'] = [];
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
        <Layout className="h-screen overflow-hidden">
            <Sider width={220} collapsible collapsed={collapsed} trigger={null}>
                <div className="flex h-16 items-center justify-center px-3 text-base font-semibold text-white">
                    {collapsed ? 'IoT' : APP_NAME}
                </div>
                <Menu
                    theme="dark"
                    mode="inline"
                    selectedKeys={[location.pathname]}
                    defaultOpenKeys={['/system']}
                    items={menuItems}
                    onClick={({ key }) => navigate(key)}
                />
            </Sider>
            <Layout className="min-w-0">
                <Header className="flex h-16 items-center justify-between bg-white px-4 shadow-sm">
                    <Button
                        type="text"
                        icon={collapsed ? <MenuUnfoldOutlined /> : <MenuFoldOutlined />}
                        onClick={() => setCollapsed((value) => !value)}
                    />
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
                        <Button>
                            <Space>
                                <UserOutlined />
                                {user?.nickname || user?.username}
                            </Space>
                        </Button>
                    </Dropdown>
                </Header>
                <Content className="m-4 min-h-0 overflow-auto rounded-lg bg-white">
                    <Outlet />
                </Content>
            </Layout>
        </Layout>
    );
}
