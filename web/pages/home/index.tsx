import { Card, Col, Row, Statistic, Typography } from 'antd';
import { useMemo } from 'react';
import { PageContainer } from '@/components/PageContainer';
import { useCurrentUser } from '@/pages/login';

const { Title, Paragraph, Text } = Typography;

const ACCOUNT_STATS = [
    {
        key: 'username',
        title: '当前账号',
        getValue: (user: ReturnType<typeof useCurrentUser>['data']) => user?.username || '-',
    },
    {
        key: 'roleCount',
        title: '角色数量',
        getValue: (user: ReturnType<typeof useCurrentUser>['data']) => user?.roles?.length ?? 0,
        suffix: '个',
    },
    {
        key: 'permissionCount',
        title: '权限数量',
        getValue: (user: ReturnType<typeof useCurrentUser>['data']) =>
            user?.permissions?.length ?? 0,
        suffix: '项',
    },
] as const;

const USER_INFO_FIELDS = [
    {
        key: 'username',
        label: '用户名',
        getValue: (user: ReturnType<typeof useCurrentUser>['data']) => user?.username || '-',
    },
    {
        key: 'nickname',
        label: '昵称',
        getValue: (user: ReturnType<typeof useCurrentUser>['data']) => user?.nickname || '-',
    },
    {
        key: 'status',
        label: '账号状态',
        getValue: (user: ReturnType<typeof useCurrentUser>['data']) =>
            user?.status === 'enabled' ? '正常' : '已禁用',
    },
    {
        key: 'roles',
        label: '角色',
        getValue: (user: ReturnType<typeof useCurrentUser>['data']) =>
            user?.roles?.map((r) => r.name).join('、') || '暂无角色',
    },
] as const;

export default function HomePage() {
    const { data: user } = useCurrentUser();

    const currentTime = useMemo(() => {
        const now = new Date();
        const hour = now.getHours();
        let greeting = '你好';
        if (hour < 6) greeting = '夜深了';
        else if (hour < 9) greeting = '早上好';
        else if (hour < 12) greeting = '上午好';
        else if (hour < 14) greeting = '中午好';
        else if (hour < 18) greeting = '下午好';
        else if (hour < 22) greeting = '晚上好';
        else greeting = '夜深了';
        return greeting;
    }, []);

    return (
        <PageContainer>
            <div
                style={{
                    display: 'flex',
                    flexDirection: 'column',
                    gap: 24,
                }}
            >
                <div>
                    <Title level={2} style={{ marginBottom: 8 }}>
                        {currentTime}，{user?.nickname || user?.username || '管理员'}
                    </Title>
                    <Paragraph style={{ marginBottom: 0, color: 'rgba(0, 0, 0, 0.65)' }}>
                        欢迎使用 IoT Engine 管理平台。
                    </Paragraph>
                </div>

                <Row gutter={[16, 16]}>
                    {ACCOUNT_STATS.map((stat) => (
                        <Col key={stat.key} xs={24} sm={12} lg={8}>
                            <Card>
                                <Statistic
                                    title={stat.title}
                                    value={stat.getValue(user)}
                                    suffix={'suffix' in stat ? stat.suffix : undefined}
                                />
                            </Card>
                        </Col>
                    ))}
                </Row>

                <Card title="当前身份信息">
                    <div
                        style={{
                            display: 'grid',
                            gridTemplateColumns: 'repeat(auto-fill, minmax(200px, 1fr))',
                            gap: 8,
                        }}
                    >
                        {USER_INFO_FIELDS.map(({ key, label, getValue }) => (
                            <Text key={key}>
                                {label}：{getValue(user)}
                            </Text>
                        ))}
                    </div>
                </Card>

                <Card title="快捷操作">
                    <Paragraph type="secondary" style={{ marginBottom: 16 }}>
                        常用操作入口，你可以根据权限访问以下功能。
                    </Paragraph>
                    <Row gutter={[12, 12]}>
                        <Col xs={24} sm={12} md={8}>
                            <Text type="secondary">用户管理</Text>
                        </Col>
                    </Row>
                </Card>

                <Card title="系统信息">
                    <div
                        style={{
                            display: 'grid',
                            gridTemplateColumns: 'repeat(auto-fill, minmax(200px, 1fr))',
                            gap: 8,
                        }}
                    >
                        <Text type="secondary">系统版本：v1.0.0</Text>
                        <Text type="secondary">前端框架：React 19</Text>
                        <Text type="secondary">UI 组件库：Ant Design 6</Text>
                        <Text type="secondary">构建工具：Vite 8</Text>
                    </div>
                </Card>
            </div>
        </PageContainer>
    );
}
