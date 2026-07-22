import {
    ApiOutlined,
    CopyOutlined,
    KeyOutlined,
    PlusOutlined,
    ReloadOutlined,
    SendOutlined,
} from '@ant-design/icons';
import {
    Alert,
    App,
    Button,
    Descriptions,
    Form,
    Input,
    InputNumber,
    Pagination,
    Result,
    Select,
    Space,
    Table,
    Tabs,
    Tag,
    Tooltip,
    Typography,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { StatusTag } from '@/components/StatusTag';
import { usePermissions } from '@/hooks/usePermission';
import { validateForm } from '@/utils/validation';
import { keySaveSchema, webhookFormSchema } from './open-access.schema';
import {
    useKeyCreate,
    useKeyDelete,
    useKeyRotate,
    useKeyUpdate,
    useOpenDevices,
    useOpenKeys,
    useOpenLogs,
    useOpenWebhooks,
    useWebhookCreate,
    useWebhookDelete,
    useWebhookUpdate,
} from './open-access.service';
import type { OpenAccess } from './open-access.types';

const { Text, Paragraph } = Typography;
const { TextArea } = Input;

const scopeOptions: Array<{ value: OpenAccess.Scope; label: string }> = [
    { value: 'device:realtime', label: '读取实时数据' },
    { value: 'device:history', label: '读取历史数据' },
    { value: 'device:command', label: '下发设备命令' },
    { value: 'alert:read', label: '读取告警' },
];
const scopeLabels = Object.fromEntries(scopeOptions.map((item) => [item.value, item.label]));
const eventOptions: Array<{ value: OpenAccess.EventType; label: string }> = [
    { value: 'device.data.reported', label: '数据上报' },
    { value: 'device.image.reported', label: '图片上报' },
    { value: 'device.command.dispatched', label: '命令已下发' },
    { value: 'device.command.responded', label: '命令已响应' },
    { value: 'device.alert.triggered', label: '告警触发' },
    { value: 'device.alert.resolved', label: '告警解除' },
];
const eventLabels = Object.fromEntries(eventOptions.map((item) => [item.value, item.label]));

type KeyFormValues = OpenAccess.KeySaveDto;
type WebhookFormValues = Omit<OpenAccess.WebhookSaveDto, 'headers'> & { headersText: string };

function dateText(value: string | null | undefined) {
    return value ? new Date(value).toLocaleString() : '-';
}

function AccessKeyPanel({ onSecret }: { onSecret: (secret: OpenAccess.KeySecret) => void }) {
    const [open, setOpen] = useState(false);
    const [editing, setEditing] = useState<OpenAccess.KeyItem | null>(null);
    const [form] = Form.useForm<KeyFormValues>();
    const selectedScopes = Form.useWatch('scopes', form) ?? [];
    const { modal } = App.useApp();
    const { has } = usePermissions();
    const { data = [], isLoading } = useOpenKeys();
    const { data: devices = [] } = useOpenDevices();
    const create = useKeyCreate();
    const update = useKeyUpdate();
    const rotate = useKeyRotate();
    const remove = useKeyDelete();

    const showCreate = () => {
        setEditing(null);
        form.setFieldsValue({
            name: '',
            status: 'enabled',
            scopes: ['device:realtime'],
            deviceIds: [],
            expiresAt: null,
            remark: null,
        });
        setOpen(true);
    };
    const showEdit = (item: OpenAccess.KeyItem) => {
        setEditing(item);
        form.setFieldsValue({
            name: item.name,
            status: item.status,
            scopes: item.scopes,
            deviceIds: item.deviceIds,
            expiresAt: item.expiresAt,
            remark: item.remark,
        });
        setOpen(true);
    };
    const submit = (values: KeyFormValues) => {
        const validated = validateForm(form, keySaveSchema, values);
        if (!validated) return;
        if (editing) {
            update.mutate({ id: editing.id, data: validated }, { onSuccess: () => setOpen(false) });
        } else {
            create.mutate(validated, {
                onSuccess: (secret) => {
                    setOpen(false);
                    onSecret(secret);
                },
            });
        }
    };
    const rotateItem = (item: OpenAccess.KeyItem) => {
        modal.confirm({
            title: `轮换「${item.name}」的 AccessKey？`,
            content: '轮换后旧密钥立即失效，调用方必须使用新密钥。',
            okText: '确认轮换',
            onOk: () => rotate.mutateAsync(item.id).then(onSecret),
        });
    };
    const deleteItem = (item: OpenAccess.KeyItem) => {
        modal.confirm({
            title: `删除调用配置「${item.name}」？`,
            content: '关联 Webhook 会一并停用，且无法恢复。',
            okButtonProps: { danger: true },
            onOk: () => remove.mutateAsync(item.id),
        });
    };

    const columns: ColumnsType<OpenAccess.KeyItem> = [
        {
            title: '调用配置',
            key: 'name',
            width: 220,
            render: (_, item) => (
                <div>
                    <div className="font-medium">{item.name}</div>
                    <Text type="secondary" className="font-mono text-xs">
                        {item.accessKeyPrefix}…
                    </Text>
                </div>
            ),
        },
        {
            title: '开放权限',
            dataIndex: 'scopes',
            width: 300,
            render: (scopes: OpenAccess.Scope[]) => (
                <Space size={[0, 4]} wrap>
                    {scopes.map((scope) => (
                        <Tag key={scope}>{scopeLabels[scope]}</Tag>
                    ))}
                </Space>
            ),
        },
        {
            title: '范围',
            key: 'scope',
            width: 130,
            render: (_, item) => `${item.deviceIds.length} 台设备 / ${item.webhookCount} 个推送`,
        },
        { title: '状态', dataIndex: 'status', render: (value) => <StatusTag status={value} /> },
        {
            title: '最近调用',
            key: 'lastUsed',
            width: 190,
            render: (_, item) => (
                <div>
                    <div>{dateText(item.lastUsedAt)}</div>
                    <Text type="secondary" className="text-xs">
                        {item.lastUsedIp || '-'}
                    </Text>
                </div>
            ),
        },
        {
            title: '过期时间',
            dataIndex: 'expiresAt',
            width: 170,
            render: dateText,
        },
        {
            title: '操作',
            key: 'actions',
            fixed: 'right',
            width: 190,
            render: (_, item) => (
                <Space size={0}>
                    {has('iot:open-access:edit') && (
                        <Button type="link" onClick={() => showEdit(item)}>
                            编辑
                        </Button>
                    )}
                    {has('iot:open-access:edit') && (
                        <Tooltip title="生成新密钥并立即废止旧密钥">
                            <Button
                                type="link"
                                icon={<ReloadOutlined />}
                                onClick={() => rotateItem(item)}
                            >
                                轮换
                            </Button>
                        </Tooltip>
                    )}
                    {has('iot:open-access:delete') && (
                        <Button type="link" danger onClick={() => deleteItem(item)}>
                            删除
                        </Button>
                    )}
                </Space>
            ),
        },
    ];

    return (
        <div className="flex h-full min-h-0 flex-col gap-3">
            <div className="flex shrink-0 justify-end">
                {has('iot:open-access:add') && (
                    <Button type="primary" icon={<PlusOutlined />} onClick={showCreate}>
                        新建调用配置
                    </Button>
                )}
            </div>
            <Table
                rowKey="id"
                columns={columns}
                dataSource={data}
                loading={isLoading}
                pagination={false}
                sticky
                scroll={{ x: 'max-content', y: 'calc(100vh - 330px)' }}
            />
            <FormModal
                open={open}
                title={editing ? '编辑调用配置' : '新建调用配置'}
                onCancel={() => setOpen(false)}
                onOk={() => form.submit()}
                confirmLoading={create.isPending || update.isPending}
                destroyOnHidden
            >
                <Form form={form} layout="vertical" onFinish={submit}>
                    <Form.Item label="名称" name="name" required>
                        <Input maxLength={64} placeholder="例如：数据平台生产环境" />
                    </Form.Item>
                    <Form.Item label="开放权限" name="scopes" required>
                        <Select mode="multiple" options={scopeOptions} />
                    </Form.Item>
                    <Form.Item label="设备范围" name="deviceIds" required>
                        <Select
                            mode="multiple"
                            showSearch
                            optionFilterProp="label"
                            options={devices.map((device) => ({
                                value: device.id,
                                label: `${device.name} (${device.deviceCode})`,
                                disabled:
                                    selectedScopes.includes('device:command') && !device.canCommand,
                            }))}
                        />
                    </Form.Item>
                    <div className="grid grid-cols-2 gap-3">
                        <Form.Item label="状态" name="status" required>
                            <Select
                                options={[
                                    { value: 'enabled', label: '启用' },
                                    { value: 'disabled', label: '禁用' },
                                ]}
                            />
                        </Form.Item>
                        <Form.Item
                            label="过期时间"
                            name="expiresAt"
                            extra="留空表示永不过期；使用带时区的 ISO 8601"
                        >
                            <Input placeholder="2026-12-31T16:00:00Z" />
                        </Form.Item>
                    </div>
                    <Form.Item label="备注" name="remark">
                        <TextArea rows={3} maxLength={200} showCount />
                    </Form.Item>
                </Form>
            </FormModal>
        </div>
    );
}

function WebhookPanel() {
    const [open, setOpen] = useState(false);
    const [editing, setEditing] = useState<OpenAccess.WebhookItem | null>(null);
    const [keyFilter, setKeyFilter] = useState<string>();
    const [form] = Form.useForm<WebhookFormValues>();
    const { modal } = App.useApp();
    const { has } = usePermissions();
    const { data: keys = [] } = useOpenKeys();
    const { data = [], isLoading } = useOpenWebhooks(keyFilter);
    const create = useWebhookCreate();
    const update = useWebhookUpdate();
    const remove = useWebhookDelete();

    const showCreate = () => {
        setEditing(null);
        form.setFieldsValue({
            accessKeyId: keyFilter ?? keys[0]?.id,
            name: '',
            url: '',
            status: 'enabled',
            timeoutSeconds: 5,
            headersText: '{}',
            eventTypes: ['device.data.reported'],
            secret: undefined,
        });
        setOpen(true);
    };
    const showEdit = (item: OpenAccess.WebhookItem) => {
        setEditing(item);
        form.setFieldsValue({
            accessKeyId: item.accessKeyId,
            name: item.name,
            url: item.url,
            status: item.status,
            timeoutSeconds: item.timeoutSeconds,
            headersText: JSON.stringify(item.headers, null, 2),
            eventTypes: item.eventTypes,
            secret: undefined,
        });
        setOpen(true);
    };
    const submit = (values: WebhookFormValues) => {
        const normalized = editing && !values.secret ? { ...values, secret: undefined } : values;
        const validated = validateForm(form, webhookFormSchema, normalized);
        if (!validated) return;
        if (editing) {
            update.mutate({ id: editing.id, data: validated }, { onSuccess: () => setOpen(false) });
        } else {
            create.mutate(validated, { onSuccess: () => setOpen(false) });
        }
    };
    const deleteItem = (item: OpenAccess.WebhookItem) => {
        modal.confirm({
            title: `删除 Webhook「${item.name}」？`,
            okButtonProps: { danger: true },
            onOk: () => remove.mutateAsync(item.id),
        });
    };
    const columns: ColumnsType<OpenAccess.WebhookItem> = [
        {
            title: 'Webhook',
            key: 'webhook',
            width: 260,
            render: (_, item) => (
                <div>
                    <div className="font-medium">{item.name}</div>
                    <Tooltip title={item.url} overlayStyle={{ maxWidth: 'none' }}>
                        <Text type="secondary" className="block max-w-64 truncate text-xs">
                            {item.url}
                        </Text>
                    </Tooltip>
                </div>
            ),
        },
        { title: '调用配置', dataIndex: 'accessKeyName', width: 180 },
        {
            title: '订阅事件',
            dataIndex: 'eventTypes',
            width: 330,
            render: (events: OpenAccess.EventType[]) => (
                <Space size={[0, 4]} wrap>
                    {events.map((event) => (
                        <Tag key={event} color="blue">
                            {eventLabels[event]}
                        </Tag>
                    ))}
                </Space>
            ),
        },
        { title: '状态', dataIndex: 'status', render: (value) => <StatusTag status={value} /> },
        {
            title: '最近投递',
            key: 'delivery',
            width: 210,
            render: (_, item) => (
                <div>
                    <div>{dateText(item.lastTriggeredAt)}</div>
                    {item.lastHttpStatus ? (
                        <Tag color={item.lastHttpStatus < 300 ? 'success' : 'error'}>
                            HTTP {item.lastHttpStatus}
                        </Tag>
                    ) : (
                        <Text type="secondary">尚未投递</Text>
                    )}
                    {item.lastError && (
                        <Tooltip title={item.lastError} overlayStyle={{ maxWidth: 'none' }}>
                            <Text type="danger" className="ml-1">
                                详情
                            </Text>
                        </Tooltip>
                    )}
                </div>
            ),
        },
        {
            title: '操作',
            key: 'actions',
            fixed: 'right',
            width: 120,
            render: (_, item) => (
                <Space size={0}>
                    {has('iot:open-access:edit') && (
                        <Button type="link" onClick={() => showEdit(item)}>
                            编辑
                        </Button>
                    )}
                    {has('iot:open-access:delete') && (
                        <Button type="link" danger onClick={() => deleteItem(item)}>
                            删除
                        </Button>
                    )}
                </Space>
            ),
        },
    ];

    return (
        <div className="flex h-full min-h-0 flex-col gap-3">
            <div className="flex shrink-0 flex-wrap justify-between gap-2">
                <Select
                    allowClear
                    className="w-64"
                    placeholder="筛选调用配置"
                    value={keyFilter}
                    onChange={setKeyFilter}
                    options={keys.map((item) => ({ value: item.id, label: item.name }))}
                />
                {has('iot:open-access:add') && (
                    <Button
                        type="primary"
                        icon={<PlusOutlined />}
                        disabled={!keys.length}
                        onClick={showCreate}
                    >
                        新建 Webhook
                    </Button>
                )}
            </div>
            <Table
                rowKey="id"
                columns={columns}
                dataSource={data}
                loading={isLoading}
                pagination={false}
                sticky
                scroll={{ x: 'max-content', y: 'calc(100vh - 330px)' }}
            />
            <FormModal
                open={open}
                title={editing ? '编辑 Webhook' : '新建 Webhook'}
                onCancel={() => setOpen(false)}
                onOk={() => form.submit()}
                confirmLoading={create.isPending || update.isPending}
                destroyOnHidden
            >
                <Form form={form} layout="vertical" onFinish={submit}>
                    <Form.Item label="调用配置" name="accessKeyId" required>
                        <Select
                            options={keys.map((item) => ({ value: item.id, label: item.name }))}
                        />
                    </Form.Item>
                    <Form.Item label="名称" name="name" required>
                        <Input maxLength={64} />
                    </Form.Item>
                    <Form.Item label="接收地址" name="url" required>
                        <Input placeholder="https://example.com/iot/events" />
                    </Form.Item>
                    <Form.Item label="订阅事件" name="eventTypes" required>
                        <Select mode="multiple" options={eventOptions} />
                    </Form.Item>
                    <div className="grid grid-cols-2 gap-3">
                        <Form.Item label="状态" name="status" required>
                            <Select
                                options={[
                                    { value: 'enabled', label: '启用' },
                                    { value: 'disabled', label: '禁用' },
                                ]}
                            />
                        </Form.Item>
                        <Form.Item label="超时（秒）" name="timeoutSeconds" required>
                            <InputNumber min={1} max={30} className="!w-full" />
                        </Form.Item>
                    </div>
                    <Form.Item
                        label="签名密钥"
                        name="secret"
                        extra={
                            editing
                                ? '留空表示保留现有密钥；传 null 可由 API 清空'
                                : '可选；用于 HMAC-SHA256 签名'
                        }
                    >
                        <Input.Password maxLength={255} autoComplete="new-password" />
                    </Form.Item>
                    <Form.Item label="自定义 Header（JSON）" name="headersText">
                        <TextArea rows={5} className="font-mono" />
                    </Form.Item>
                </Form>
            </FormModal>
        </div>
    );
}

function LogPanel() {
    const [query, setQuery] = useState<OpenAccess.LogQuery>({ page: 1, pageSize: 20 });
    const { data: keys = [] } = useOpenKeys();
    const { data, isLoading } = useOpenLogs(query);
    const columns: ColumnsType<OpenAccess.LogItem> = [
        { title: '时间', dataIndex: 'createdAt', width: 180, render: dateText },
        {
            title: '方向',
            dataIndex: 'direction',
            width: 90,
            render: (value) => <Tag color={value === 'push' ? 'blue' : 'purple'}>{value}</Tag>,
        },
        {
            title: '动作 / 事件',
            key: 'action',
            width: 230,
            render: (_, item) => item.eventType || item.action,
        },
        {
            title: '调用配置',
            dataIndex: 'accessKeyName',
            width: 160,
            render: (value) => value || '-',
        },
        { title: '设备', dataIndex: 'deviceCode', width: 150, render: (value) => value || '-' },
        {
            title: '结果',
            key: 'result',
            width: 120,
            render: (_, item) => (
                <Tag color={item.status === 'success' ? 'success' : 'error'}>
                    {item.httpStatus || item.status}
                </Tag>
            ),
        },
        {
            title: '目标 / 来源',
            key: 'target',
            width: 300,
            render: (_, item) => (
                <Tooltip
                    title={item.target || item.requestIp || '-'}
                    overlayStyle={{ maxWidth: 'none' }}
                >
                    <Text className="block max-w-72 truncate">
                        {item.target || item.requestIp || '-'}
                    </Text>
                </Tooltip>
            ),
        },
        { title: '消息', dataIndex: 'message', width: 240, render: (value) => value || '-' },
    ];
    return (
        <div className="flex h-full min-h-0 flex-col gap-3">
            <Space className="shrink-0" wrap>
                <Select
                    allowClear
                    className="w-56"
                    placeholder="调用配置"
                    onChange={(accessKeyId) =>
                        setQuery((value) => ({ ...value, page: 1, accessKeyId }))
                    }
                    options={keys.map((item) => ({ value: item.id, label: item.name }))}
                />
                <Select
                    allowClear
                    className="w-32"
                    placeholder="方向"
                    onChange={(direction) =>
                        setQuery((value) => ({ ...value, page: 1, direction }))
                    }
                    options={[
                        { value: 'pull', label: 'API 调用' },
                        { value: 'push', label: 'Webhook' },
                    ]}
                />
                <Select
                    allowClear
                    className="w-32"
                    placeholder="结果"
                    onChange={(status) => setQuery((value) => ({ ...value, page: 1, status }))}
                    options={[
                        { value: 'success', label: '成功' },
                        { value: 'failed', label: '失败' },
                    ]}
                />
            </Space>
            <Table
                rowKey="id"
                columns={columns}
                dataSource={data?.list ?? []}
                loading={isLoading}
                pagination={false}
                sticky
                expandable={{
                    expandedRowRender: (item) => (
                        <Descriptions size="small" column={1} bordered>
                            <Descriptions.Item label="请求">
                                <Paragraph
                                    copyable
                                    className="m-0 whitespace-pre-wrap font-mono text-xs"
                                >
                                    {JSON.stringify(item.requestPayload, null, 2)}
                                </Paragraph>
                            </Descriptions.Item>
                            <Descriptions.Item label="响应">
                                <Paragraph
                                    copyable
                                    className="m-0 whitespace-pre-wrap font-mono text-xs"
                                >
                                    {JSON.stringify(item.responsePayload, null, 2)}
                                </Paragraph>
                            </Descriptions.Item>
                        </Descriptions>
                    ),
                }}
                scroll={{ x: 'max-content', y: 'calc(100vh - 380px)' }}
            />
            <div className="flex shrink-0 justify-end">
                <Pagination
                    current={Number(query.page)}
                    pageSize={Number(query.pageSize)}
                    total={data?.total ?? 0}
                    showSizeChanger
                    showTotal={(total) => `共 ${total} 条`}
                    onChange={(page, pageSize) =>
                        setQuery((value) => ({ ...value, page, pageSize }))
                    }
                />
            </div>
        </div>
    );
}

export default function OpenAccessPage() {
    const [secret, setSecret] = useState<OpenAccess.KeySecret | null>(null);
    const { has } = usePermissions();
    if (!has('iot:open-access:query')) {
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询开放接入配置的权限" />
            </PageContainer>
        );
    }
    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-start justify-between gap-3">
                    <div>
                        <h2 className="m-0 text-lg font-semibold">开放接入</h2>
                        <p className="m-0 mt-1 text-xs text-slate-500">
                            通过设备范围与最小权限控制外部 API，并异步投递设备事件
                        </p>
                    </div>
                    <Space wrap>
                        <Tag icon={<ApiOutlined />} color="blue">
                            /open-api
                        </Tag>
                        <Tag icon={<KeyOutlined />}>X-Access-Key</Tag>
                    </Space>
                </div>
            }
        >
            <div className="flex h-full min-h-0 flex-col gap-3">
                {secret && (
                    <Alert
                        className="shrink-0"
                        type="warning"
                        showIcon
                        closable
                        onClose={() => setSecret(null)}
                        message="AccessKey 仅显示一次，请立即保存"
                        description={
                            <Text
                                copyable={{ icon: <CopyOutlined /> }}
                                className="break-all font-mono"
                            >
                                {secret.accessKey}
                            </Text>
                        }
                    />
                )}
                <Alert
                    className="shrink-0"
                    type="info"
                    showIcon
                    message="原生开放 API"
                    description={
                        <Space size={[12, 4]} wrap>
                            <Text code>GET /open-api/devices</Text>
                            <Text code>GET /open-api/devices/:id/realtime</Text>
                            <Text code>GET /open-api/devices/:id/history</Text>
                            <Text code>POST /open-api/devices/:id/commands</Text>
                            <Text code>GET /open-api/alerts</Text>
                        </Space>
                    }
                />
                <Tabs
                    className="min-h-0 flex-1 [&_.ant-tabs-content]:h-full [&_.ant-tabs-content-holder]:min-h-0 [&_.ant-tabs-tabpane]:h-full"
                    items={[
                        {
                            key: 'keys',
                            label: (
                                <span>
                                    <KeyOutlined /> 调用配置
                                </span>
                            ),
                            children: <AccessKeyPanel onSecret={setSecret} />,
                        },
                        {
                            key: 'webhooks',
                            label: (
                                <span>
                                    <SendOutlined /> Webhook
                                </span>
                            ),
                            children: <WebhookPanel />,
                        },
                        { key: 'logs', label: '调用日志', children: <LogPanel /> },
                    ]}
                />
            </div>
        </PageContainer>
    );
}
