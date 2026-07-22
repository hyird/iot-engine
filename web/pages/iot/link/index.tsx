import {
    App,
    Button,
    Form,
    Input,
    InputNumber,
    Result,
    Select,
    Space,
    Table,
    Tag,
    Tooltip,
} from 'antd';
import type { ColumnsType, TablePaginationConfig } from 'antd/es/table';
import { useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { StatusTag } from '@/components/StatusTag';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { validateForm } from '@/utils/validation';
import { saveLinkSchema } from './link.schema';
import { useLinkDelete, useLinkEnums, useLinkList, useLinkSave, usePublicIp } from './link.service';
import type { Link } from './link.types';

const { Search } = Input;

const tooltipStyles = {
    root: { maxWidth: 'none' },
    container: { maxWidth: 'none', whiteSpace: 'nowrap' },
} as const;

type LinkFormValues = Link.SaveDto & { id?: string };

const connectionLabels: Record<Link.Item['conn_status'], { color: string; text: string }> = {
    stopped: { color: 'default', text: '已停止' },
    listening: { color: 'processing', text: '监听中' },
    connected: { color: 'success', text: '已连接' },
    partial: { color: 'warning', text: '部分连接' },
    connecting: { color: 'warning', text: '连接中' },
    reconnecting: { color: 'warning', text: '重连中' },
    error: { color: 'error', text: '错误' },
};

const createTarget = (index = 1): Link.Target => ({
    id: `target-${Date.now()}-${Math.random().toString(16).slice(2)}`,
    name: `目标${index}`,
    ip: '',
    port: 502,
    status: 'enabled',
});

function formatDateTime(value: string) {
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? value : date.toLocaleString('zh-CN', { hour12: false });
}

export default function IotLinkPage() {
    const [keyword, setKeyword] = useState('');
    const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
    const [modalVisible, setModalVisible] = useState(false);
    const [editing, setEditing] = useState<Link.Item | null>(null);
    const [form] = Form.useForm<LinkFormValues>();
    const selectedMode = Form.useWatch('mode', form) as Link.Mode | undefined;
    const { modal } = App.useApp();
    const { has } = usePermissions();
    const canQuery = has('iot:link:query');
    const canAdd = has('iot:link:add');
    const canEdit = has('iot:link:edit');
    const canDelete = has('iot:link:delete');

    const doSearch = (value: string) => {
        setKeyword(value);
        setPagination((current) => ({ ...current, page: 1 }));
    };
    const { run: debouncedSearch } = useDebounceFn(doSearch, 300);
    const { data: linkEnums } = useLinkEnums({ enabled: canQuery });
    const { data: publicIp } = usePublicIp({ enabled: canQuery });
    const { data, isLoading } = useLinkList(
        { ...pagination, keyword: keyword || undefined },
        { enabled: canQuery, refetchInterval: 3000 }
    );
    const save = useLinkSave();
    const remove = useLinkDelete();

    const openCreateModal = () => {
        setEditing(null);
        form.resetFields();
        form.setFieldsValue({
            status: 'enabled',
            mode: 'TCP Client',
            protocol: 'SL651',
            ip: '',
            port: 0,
            targets: [createTarget()],
        });
        setModalVisible(true);
    };

    const handleModeChange = (mode: Link.Mode) => {
        if (mode === 'TCP Server') {
            form.setFieldsValue({ ip: '0.0.0.0', port: 502, targets: [] });
        } else {
            if (form.getFieldValue('protocol') === 'SL651')
                form.setFieldValue('protocol', 'Modbus');
            form.setFieldsValue({ ip: '', port: 0, targets: [createTarget()] });
        }
    };

    const openEditModal = (record: Link.Item) => {
        setEditing(record);
        form.setFieldsValue({
            id: record.id,
            name: record.name,
            mode: record.mode,
            protocol: record.protocol,
            ip: record.mode === 'TCP Server' ? '0.0.0.0' : '',
            port: record.port,
            targets: record.targets.map((target) => ({ ...target })),
            status: record.status,
        });
        setModalVisible(true);
    };

    const onDelete = (record: Link.Item) =>
        modal.confirm({
            title: `确认删除链路「${record.name}」吗？`,
            content: '删除后该链路下的所有设备将无法通信。此操作不可撤销。',
            okText: '确定删除',
            okButtonProps: { danger: true },
            onOk: () => remove.mutate(record.id),
        });

    const onFinish = (values: LinkFormValues) => {
        const payload: Link.SaveDto = {
            ...values,
            ip: values.mode === 'TCP Server' ? '0.0.0.0' : '',
            port: values.mode === 'TCP Server' ? values.port : 0,
            targets: values.mode === 'TCP Client' ? values.targets : [],
        };
        const validated = validateForm(form, saveLinkSchema, payload);
        if (!validated) return;
        save.mutate(editing ? { ...validated, id: editing.id } : validated, {
            onSuccess: () => {
                setModalVisible(false);
                setEditing(null);
            },
        });
    };

    const handleTableChange = (config: TablePaginationConfig) =>
        setPagination({ page: config.current ?? 1, pageSize: config.pageSize ?? 10 });

    if (!canQuery)
        return (
            <PageContainer>
                <Result
                    status="403"
                    title="无权限"
                    subTitle="您没有查询链路列表的权限，请联系管理员"
                />
            </PageContainer>
        );

    const columns: ColumnsType<Link.Item> = [
        { title: '链路名称', dataIndex: 'name' },
        { title: '模式', dataIndex: 'mode' },
        { title: '协议', dataIndex: 'protocol' },
        {
            title: '监听 / 目标地址',
            key: 'endpoint',
            render: (_, record) =>
                record.mode === 'TCP Server' ? (
                    `${record.ip}:${record.port}`
                ) : (
                    <Tooltip
                        styles={tooltipStyles}
                        title={record.targets.map((target) => (
                            <div key={target.id}>
                                {target.name}: {target.ip}:{target.port}
                            </div>
                        ))}
                    >
                        <Tag color="blue">{record.targets.length} 个目标</Tag>
                    </Tooltip>
                ),
        },
        {
            title: '启用',
            dataIndex: 'status',
            render: (value: Link.Status) => <StatusTag status={value} />,
        },
        {
            title: '连接状态',
            key: 'conn_status',
            render: (_, record) => {
                const display = connectionLabels[record.conn_status] ?? connectionLabels.stopped;
                if (record.mode === 'TCP Server' && record.conn_status === 'listening') {
                    const clientCount = (
                        <Tag color="blue" className={record.client_count ? 'cursor-pointer' : ''}>
                            {record.client_count} 客户端
                        </Tag>
                    );
                    return (
                        <Space size={4}>
                            <Tag color={display.color}>{display.text}</Tag>
                            {record.client_count ? (
                                <Tooltip
                                    styles={tooltipStyles}
                                    title={
                                        <div>
                                            <div className="mb-1 font-medium">已连接客户端：</div>
                                            {record.clients?.map((endpoint) => (
                                                <div key={endpoint}>{endpoint}</div>
                                            ))}
                                        </div>
                                    }
                                >
                                    {clientCount}
                                </Tooltip>
                            ) : (
                                clientCount
                            )}
                        </Space>
                    );
                }
                if (record.mode === 'TCP Client') {
                    const enabledTargetCount = record.targets.filter(
                        (target) => target.status === 'enabled'
                    ).length;
                    return (
                        <Space size={4}>
                            <Tooltip
                                styles={tooltipStyles}
                                title={record.targets
                                    .filter((target) => target.status === 'enabled')
                                    .map((target) => (
                                        <div key={target.id}>
                                            {target.name}（{target.ip}:{target.port}）：
                                            {connectionLabels[target.conn_status ?? 'stopped'].text}
                                            {target.error_msg ? `（${target.error_msg}）` : ''}
                                            {target.last_activity_at_ms
                                                ? `，最后活动 ${formatDateTime(
                                                      new Date(
                                                          target.last_activity_at_ms
                                                      ).toISOString()
                                                  )}`
                                                : ''}
                                        </div>
                                    ))}
                            >
                                <Tag color={display.color}>{display.text}</Tag>
                            </Tooltip>
                            <Tag color="blue">
                                {record.client_count}/{enabledTargetCount} 服务端
                            </Tag>
                        </Space>
                    );
                }
                return <Tag color={display.color}>{display.text}</Tag>;
            },
        },
        {
            title: '创建时间',
            dataIndex: 'created_at',
            width: 180,
            render: (value: string) => formatDateTime(value),
        },
        {
            title: '操作',
            key: 'actions',
            width: 150,
            fixed: 'right',
            render: (_, record) => (
                <Space>
                    {canEdit && (
                        <Button type="link" onClick={() => openEditModal(record)}>
                            编辑
                        </Button>
                    )}
                    {canDelete && (
                        <Button type="link" danger onClick={() => onDelete(record)}>
                            删除
                        </Button>
                    )}
                </Space>
            ),
        },
    ];

    const modes = linkEnums?.modes ?? ['TCP Server', 'TCP Client'];
    const protocols = linkEnums?.protocols ?? ['SL651', 'Modbus', 'S7'];
    const availableProtocols =
        selectedMode === 'TCP Client'
            ? protocols.filter((protocol) => protocol !== 'SL651')
            : protocols;

    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-center justify-between gap-2">
                    <Space wrap>
                        <h3 className="m-0 text-base font-medium">链路管理</h3>
                        {publicIp?.ip ? <Tag color="blue">公网 IP: {publicIp.ip}</Tag> : null}
                    </Space>
                    <Space wrap>
                        <Search
                            allowClear
                            className="w-60"
                            placeholder="链路名称 / IP地址"
                            onChange={(event) => debouncedSearch(event.target.value)}
                            onSearch={doSearch}
                        />
                        {canAdd && (
                            <Button type="primary" onClick={openCreateModal}>
                                新建链路
                            </Button>
                        )}
                    </Space>
                </div>
            }
        >
            <Table<Link.Item>
                rowKey="id"
                columns={columns}
                dataSource={data?.list ?? []}
                loading={isLoading}
                pagination={{
                    current: pagination.page,
                    pageSize: pagination.pageSize,
                    total: data?.total ?? 0,
                    showSizeChanger: true,
                    showTotal: (total, range) => `${range[0]}-${range[1]} / 共 ${total} 条`,
                }}
                onChange={handleTableChange}
                size="middle"
                sticky
                scroll={{ x: 'max-content', y: 'calc(100vh - 280px)' }}
            />

            <FormModal
                open={modalVisible}
                title={editing ? '编辑链路' : '新建链路'}
                onCancel={() => {
                    setModalVisible(false);
                    setEditing(null);
                }}
                onOk={() => form.submit()}
                confirmLoading={save.isPending}
                afterOpenChange={(open) => {
                    if (!open) form.resetFields();
                }}
                destroyOnHidden
            >
                <Form<LinkFormValues> form={form} layout="vertical" onFinish={onFinish}>
                    <Form.Item name="id" hidden>
                        <Input />
                    </Form.Item>
                    <Form.Item
                        label="链路名称"
                        name="name"
                        rules={[{ required: true, message: '请输入链路名称' }]}
                    >
                        <Input placeholder="链路名称" />
                    </Form.Item>
                    <Form.Item
                        label="模式"
                        name="mode"
                        rules={[{ required: true, message: '请选择模式' }]}
                        extra={editing ? '链路创建后模式不可修改' : undefined}
                    >
                        <Select
                            disabled={Boolean(editing)}
                            options={modes.map((value) => ({ value, label: value }))}
                            onChange={handleModeChange}
                        />
                    </Form.Item>
                    <Form.Item
                        label="协议"
                        name="protocol"
                        rules={[{ required: true, message: '请选择协议' }]}
                        extra={editing ? '链路创建后协议不可修改' : undefined}
                    >
                        <Select
                            disabled={Boolean(editing)}
                            options={availableProtocols.map((value) => ({ value, label: value }))}
                        />
                    </Form.Item>
                    <Form.Item
                        noStyle
                        shouldUpdate={(previous, next) => previous.mode !== next.mode}
                    >
                        {({ getFieldValue }) =>
                            getFieldValue('mode') === 'TCP Server' ? (
                                <div className="grid grid-cols-2 gap-3">
                                    <Form.Item
                                        label="监听IP"
                                        name="ip"
                                        rules={[{ required: true, message: '请输入监听IP' }]}
                                    >
                                        <Input placeholder="0.0.0.0" disabled />
                                    </Form.Item>
                                    <Form.Item
                                        label="监听端口"
                                        name="port"
                                        rules={[
                                            { required: true, message: '请输入监听端口' },
                                            {
                                                type: 'number',
                                                min: 1,
                                                max: 65535,
                                                message: '端口范围 1-65535',
                                            },
                                        ]}
                                    >
                                        <InputNumber
                                            className="!w-full"
                                            min={1}
                                            max={65535}
                                            placeholder="如: 8080"
                                        />
                                    </Form.Item>
                                </div>
                            ) : (
                                <Form.List
                                    name="targets"
                                    rules={[
                                        {
                                            validator: async (_, targets) => {
                                                if (!targets?.length)
                                                    throw new Error('至少配置一个目标地址');
                                            },
                                        },
                                    ]}
                                >
                                    {(fields, { add, remove: removeTarget }, { errors }) => (
                                        <div>
                                            <div className="mb-2 flex items-center justify-between">
                                                <span>目标地址</span>
                                                <Button
                                                    type="dashed"
                                                    onClick={() =>
                                                        add(createTarget(fields.length + 1))
                                                    }
                                                >
                                                    添加目标
                                                </Button>
                                            </div>
                                            {fields.map((field) => (
                                                <div
                                                    key={field.key}
                                                    className="mb-3 grid grid-cols-[1fr_1.25fr_110px_100px_auto] items-start gap-2 rounded-lg border border-gray-200 p-3"
                                                >
                                                    <Form.Item name={[field.name, 'id']} hidden>
                                                        <Input />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'name']}
                                                        rules={[
                                                            {
                                                                required: true,
                                                                message: '请输入名称',
                                                            },
                                                        ]}
                                                    >
                                                        <Input placeholder="目标名称" />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'ip']}
                                                        rules={[
                                                            {
                                                                required: true,
                                                                message: '请输入目标IP',
                                                            },
                                                            {
                                                                pattern: /^(\d{1,3}\.){3}\d{1,3}$/,
                                                                message: 'IPv4格式错误',
                                                            },
                                                        ]}
                                                    >
                                                        <Input placeholder="192.168.1.100" />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'port']}
                                                        rules={[
                                                            {
                                                                required: true,
                                                                message: '请输入端口',
                                                            },
                                                            {
                                                                type: 'number',
                                                                min: 1,
                                                                max: 65535,
                                                                message: '1-65535',
                                                            },
                                                        ]}
                                                    >
                                                        <InputNumber
                                                            className="!w-full"
                                                            min={1}
                                                            max={65535}
                                                        />
                                                    </Form.Item>
                                                    <Form.Item name={[field.name, 'status']}>
                                                        <Select
                                                            options={[
                                                                { value: 'enabled', label: '启用' },
                                                                {
                                                                    value: 'disabled',
                                                                    label: '禁用',
                                                                },
                                                            ]}
                                                        />
                                                    </Form.Item>
                                                    <Button
                                                        danger
                                                        type="text"
                                                        onClick={() => removeTarget(field.name)}
                                                    >
                                                        删除
                                                    </Button>
                                                </div>
                                            ))}
                                            <Form.ErrorList errors={errors} />
                                        </div>
                                    )}
                                </Form.List>
                            )
                        }
                    </Form.Item>
                    <Form.Item
                        label="状态"
                        name="status"
                        rules={[{ required: true, message: '请选择状态' }]}
                    >
                        <Select
                            options={[
                                { value: 'enabled', label: '启用' },
                                { value: 'disabled', label: '禁用' },
                            ]}
                        />
                    </Form.Item>
                </Form>
            </FormModal>
        </PageContainer>
    );
}
