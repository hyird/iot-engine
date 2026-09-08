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
import { formatDateTime } from '@/utils/dateTime';
import { validateForm } from '@/utils/validation';
import { saveLinkSchema } from './link.schema';
import { useLinkDelete, useLinkEnums, useLinkList, useLinkSave, usePublicIp } from './link.service';
import type { Link } from './link.types';
import { useEdgeInventory } from '../edge-node/edge-node.service';

const { Search } = Input;

const tooltipStyles = {
    root: { maxWidth: 'none' },
    container: { maxWidth: 'none', whiteSpace: 'nowrap' },
} as const;

type LinkFormValues = Omit<Link.SaveDto, 'endpoint'> &
    Link.Endpoint & {
        id?: string;
    };

const connectionLabels: Record<Link.Connection, { color: string; text: string }> = {
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

export default function IotLinkPage() {
    const [keyword, setKeyword] = useState('');
    const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
    const [modalVisible, setModalVisible] = useState(false);
    const [editing, setEditing] = useState<Link.Item | null>(null);
    const [form] = Form.useForm<LinkFormValues>();
    const selectedExecution = Form.useWatch('execution', form);
    const selectedTransport = Form.useWatch('transport', form);
    const { data: nodes = [] } = useEdgeInventory(modalVisible);
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
        { enabled: canQuery }
    );
    const save = useLinkSave();
    const remove = useLinkDelete();

    const openCreateModal = () => {
        setEditing(null);
        form.resetFields();
        form.setFieldsValue({
            execution: 'collector', transport:'serial', baud_rate:9600,data_bits:8,stop_bits:1,parity:'none',
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
            ...record.endpoint, execution: record.execution, edge_node_id: record.edge_node_id,
            id: record.id,
            name: record.name,
            mode: record.endpoint.mode || 'TCP Client',
            protocol: record.protocol,
            ip: record.endpoint.ip,
            port: record.endpoint.port,
            targets: record.endpoint.targets.map((target) => ({ ...target })),
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
            execution: values.execution, edge_node_id: values.edge_node_id,
            name: values.name,
            protocol: values.protocol,
            status: values.status,
            endpoint: {
                transport:values.transport,interface:values.interface,baud_rate:values.baud_rate,data_bits:values.data_bits,
                stop_bits:values.stop_bits,parity:values.parity,rs485:values.rs485,
                mode: values.mode,
                ip: values.execution === 'edge' ? (values.ip ?? '') : values.mode === 'TCP Server' ? '0.0.0.0' : '',
                port: values.execution === 'edge' ? (values.port ?? 0) : values.mode === 'TCP Server' ? values.port : 0,
                targets: values.execution !== 'edge' && values.mode === 'TCP Client' ? values.targets : [],
            },
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
        { title: '模式', key: 'mode', render: (_, record) => record.endpoint.transport === 'serial' ? '串口' : record.endpoint.mode },
        { title: '协议', dataIndex: 'protocol' },
        {
            title: '监听 / 目标地址',
            key: 'endpoint',
            render: (_, record) =>
                record.execution === 'edge' ? (
                    record.endpoint.transport === 'serial'
                        ? `${record.endpoint.interface} · ${record.endpoint.baud_rate} baud`
                        : `${record.endpoint.interface} · ${record.endpoint.ip}:${record.endpoint.port}`
                ) : record.endpoint.mode === 'TCP Server' ? (
                    `${record.endpoint.ip}:${record.endpoint.port}`
                ) : (
                    <Tooltip
                        styles={tooltipStyles}
                        title={record.endpoint.targets.map((target) => (
                            <div key={target.id}>
                                {target.name}: {target.ip}:{target.port}
                            </div>
                        ))}
                    >
                        <Tag color="blue">{record.endpoint.targets.length} 个目标</Tag>
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
                if (record.execution === 'edge') return <Tag>由边缘节点管理</Tag>;
                const runtime = record.runtime;
                const state = runtime?.state ?? 'stopped';
                const display = connectionLabels[state] ?? connectionLabels.stopped;
                if (record.endpoint.mode === 'TCP Server' && state === 'listening') {
                    const clientCount = (
                        <Tag
                            color="blue"
                            className={runtime?.clientCount ? 'cursor-pointer' : ''}
                        >
                            {runtime?.clientCount ?? 0} 客户端
                        </Tag>
                    );
                    return (
                        <Space size={4}>
                            <Tag color={display.color}>{display.text}</Tag>
                            {runtime?.clientCount ? (
                                <Tooltip
                                    styles={tooltipStyles}
                                    title={
                                        <div>
                                            <div className="mb-1 font-medium">已连接客户端：</div>
                                            {runtime.clients?.map((endpoint) => (
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
                if (record.endpoint.mode === 'TCP Client') {
                    const enabledTargetCount = record.endpoint.targets.filter(
                        (target) => target.status === 'enabled'
                    ).length;
                    return (
                        <Space size={4}>
                            <Tooltip
                                styles={tooltipStyles}
                                title={record.endpoint.targets
                                    .filter((target) => target.status === 'enabled')
                                    .map((target) => (
                                        <div key={target.id}>
                                            {target.name}（{target.ip}:{target.port}）：
                                            {
                                                connectionLabels[
                                                    target.runtime?.state ?? 'stopped'
                                                ].text
                                            }
                                            {target.runtime?.error
                                                ? `（${target.runtime.error}）`
                                                : ''}
                                            {target.runtime?.lastActivityAt
                                                ? `，最后活动 ${formatDateTime(
                                                      target.runtime.lastActivityAt
                                                  )}`
                                                : ''}
                                        </div>
                                    ))}
                            >
                                <Tag color={display.color}>{display.text}</Tag>
                            </Tooltip>
                            <Tag color="blue">
                                {runtime?.clientCount ?? 0}/{enabledTargetCount} 服务端
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
        selectedExecution !== 'edge' && selectedMode === 'TCP Client'
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
                    <Form.Item name="execution" label="采集位置" rules={[{required:true}]}><Select disabled={!!editing} options={[{value:'collector',label:'平台采集'},{value:'edge',label:'边缘节点'}]} /></Form.Item>
                    {selectedExecution === 'edge' && <>
                        <Form.Item name="edge_node_id" label="边缘节点" rules={[{required:true}]}><Select showSearch optionFilterProp="label" options={nodes.map(n => ({value:n.id,label:n.name || n.imei}))} /></Form.Item>
                        <Form.Item name="transport" label="传输类型" rules={[{required:true}]}><Select options={[{value:'serial',label:'串口'},{value:'tcp',label:'TCP'}]} /></Form.Item>
                        <Form.Item name="interface" label="节点接口" rules={[{required:true}]}><Input placeholder={selectedTransport === 'serial' ? '/dev/ttyS1' : 'br-lan'} /></Form.Item>
                        {selectedTransport === 'serial' ? <>
                            <Form.Item name="baud_rate" label="波特率"><InputNumber min={300} max={4000000} /></Form.Item>
                            <Form.Item name="data_bits" label="数据位"><InputNumber min={5} max={8} /></Form.Item>
                            <Form.Item name="stop_bits" label="停止位"><InputNumber min={1} max={2} /></Form.Item>
                            <Form.Item name="parity" label="校验"><Select options={['none','odd','even'].map(value => ({value,label:value}))} /></Form.Item>
                            <Form.Item name="rs485" label="RS485"><Select options={[{value:true,label:'启用'},{value:false,label:'停用'}]} /></Form.Item>
                        </> : <>
                            <Form.Item name="ip" label="IP 地址" rules={[{required:true}]}><Input /></Form.Item>
                            <Form.Item name="port" label="端口" rules={[{required:true}]}><InputNumber min={1} max={65535} /></Form.Item>
                        </>}
                    </>}
                    <Form.Item
                        label="模式"
                        name="mode"
                        hidden={selectedExecution === 'edge' && selectedTransport === 'serial'}
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
                    {selectedExecution !== 'edge' && <Form.Item
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
                    </Form.Item>}
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
