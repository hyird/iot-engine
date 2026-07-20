import { MinusCircleOutlined, PlusOutlined } from '@ant-design/icons';
import {
    App,
    Button,
    Form,
    Input,
    InputNumber,
    Pagination,
    Result,
    Select,
    Space,
    Table,
    Tag,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { StatusTag } from '@/components/StatusTag';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { validateForm } from '@/utils/validation';
import { saveLinkSchema } from './link.schema';
import { useLinkDelete, useLinkList, useLinkSave } from './link.service';
import type { Link } from './link.types';

const { Search } = Input;
type LinkFormValues = Link.SaveDto & { id?: number };

const newTarget = (): Link.Target => ({
    id: crypto.randomUUID(),
    name: '',
    ip: '',
    port: 0,
    status: 'enabled',
});

const connectionLabels: Record<Link.Item['conn_status'], { color: string; text: string }> = {
    stopped: { color: 'default', text: '未运行' },
    listening: { color: 'processing', text: '监听中' },
    connected: { color: 'success', text: '已连接' },
    partial: { color: 'warning', text: '部分连接' },
    connecting: { color: 'processing', text: '连接中' },
    error: { color: 'error', text: '异常' },
};

export default function IotLinkPage() {
    const [keyword, setKeyword] = useState('');
    const [mode, setMode] = useState<Link.Mode | undefined>();
    const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
    const [editing, setEditing] = useState<Link.Item | null>(null);
    const [open, setOpen] = useState(false);
    const [form] = Form.useForm<LinkFormValues>();
    const selectedMode = Form.useWatch('mode', form);
    const { modal } = App.useApp();
    const { has } = usePermissions();
    const canQuery = has('iot:link:query');
    const canAdd = has('iot:link:add');
    const canEdit = has('iot:link:edit');
    const canDelete = has('iot:link:delete');
    const { run: search } = useDebounceFn((value: string) => {
        setKeyword(value);
        setPagination((current) => ({ ...current, page: 1 }));
    }, 300);
    const { data, isLoading } = useLinkList(
        { ...pagination, keyword: keyword || undefined, mode },
        { enabled: canQuery }
    );
    const save = useLinkSave();
    const remove = useLinkDelete();

    const showCreate = () => {
        setEditing(null);
        form.resetFields();
        form.setFieldsValue({
            mode: 'TCP Server',
            protocol: 'Modbus',
            ip: '0.0.0.0',
            port: 502,
            targets: [],
            status: 'enabled',
        });
        setOpen(true);
    };
    const showEdit = (link: Link.Item) => {
        setEditing(link);
        form.setFieldsValue({ ...link, targets: link.targets.map((target) => ({ ...target })) });
        setOpen(true);
    };
    const submit = (raw: LinkFormValues) => {
        const values: LinkFormValues =
            raw.mode === 'TCP Server' ? { ...raw, targets: [] } : { ...raw, ip: '', port: 0 };
        const validated = validateForm(form, saveLinkSchema, values);
        if (!validated) return;
        save.mutate(editing ? { ...validated, id: editing.id } : validated, {
            onSuccess: () => setOpen(false),
        });
    };
    const confirmDelete = (link: Link.Item) =>
        modal.confirm({
            title: `确认删除链路「${link.name}」吗？`,
            content: '链路配置删除后将不再可用。',
            okButtonProps: { danger: true },
            onOk: () => remove.mutate(link.id),
        });

    if (!canQuery)
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询链路的权限" />
            </PageContainer>
        );

    const columns: ColumnsType<Link.Item> = [
        { title: '链路名称', dataIndex: 'name' },
        { title: '模式', dataIndex: 'mode', width: 120 },
        { title: '协议', dataIndex: 'protocol', width: 100 },
        {
            title: '地址 / 目标',
            render: (_, link) =>
                link.mode === 'TCP Server'
                    ? `${link.ip}:${link.port}`
                    : `${link.targets.length} 个目标`,
        },
        {
            title: '启用状态',
            dataIndex: 'status',
            width: 100,
            render: (value: Link.Status) => <StatusTag status={value} />,
        },
        {
            title: '连接状态',
            dataIndex: 'conn_status',
            width: 100,
            render: (value: Link.Item['conn_status']) => {
                const display = connectionLabels[value];
                return <Tag color={display.color}>{display.text}</Tag>;
            },
        },
        {
            title: '操作',
            key: 'actions',
            width: 140,
            render: (_, link) => (
                <Space>
                    {canEdit && (
                        <Button type="link" onClick={() => showEdit(link)}>
                            编辑
                        </Button>
                    )}
                    {canDelete && (
                        <Button type="link" danger onClick={() => confirmDelete(link)}>
                            删除
                        </Button>
                    )}
                </Space>
            ),
        },
    ];

    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-center justify-between gap-2">
                    <h3 className="m-0 text-base font-medium">链路管理</h3>
                    <Space>
                        <Select
                            allowClear
                            className="w-32"
                            placeholder="链路模式"
                            options={[
                                { value: 'TCP Server', label: 'TCP Server' },
                                { value: 'TCP Client', label: 'TCP Client' },
                            ]}
                            onChange={(value) => {
                                setMode(value);
                                setPagination((current) => ({ ...current, page: 1 }));
                            }}
                        />
                        <Search
                            allowClear
                            placeholder="链路名称"
                            onChange={(event) => search(event.target.value)}
                        />
                        {canAdd && (
                            <Button type="primary" onClick={showCreate}>
                                新建链路
                            </Button>
                        )}
                    </Space>
                </div>
            }
            footer={
                <div className="flex justify-end">
                    <Pagination
                        {...pagination}
                        total={data?.total || 0}
                        showSizeChanger
                        onChange={(page, pageSize) => setPagination({ page, pageSize })}
                    />
                </div>
            }
        >
            <Table
                rowKey="id"
                columns={columns}
                dataSource={data?.list || []}
                loading={isLoading}
                pagination={false}
            />
            <FormModal
                open={open}
                title={editing ? '编辑链路' : '新建链路'}
                onCancel={() => setOpen(false)}
                onOk={() => form.submit()}
                confirmLoading={save.isPending}
                destroyOnHidden
            >
                <Form form={form} layout="vertical" onFinish={submit}>
                    <Form.Item label="链路名称" name="name">
                        <Input maxLength={100} />
                    </Form.Item>
                    <div className="grid grid-cols-2 gap-3">
                        <Form.Item label="链路模式" name="mode">
                            <Select
                                disabled={Boolean(editing)}
                                options={[
                                    { value: 'TCP Server', label: 'TCP Server' },
                                    { value: 'TCP Client', label: 'TCP Client' },
                                ]}
                            />
                        </Form.Item>
                        <Form.Item label="协议" name="protocol">
                            <Select
                                disabled={Boolean(editing)}
                                options={['SL651', 'Modbus', 'S7'].map((value) => ({
                                    value,
                                    label: value,
                                }))}
                            />
                        </Form.Item>
                    </div>
                    {selectedMode === 'TCP Server' ? (
                        <div className="grid grid-cols-2 gap-3">
                            <Form.Item label="监听 IP" name="ip">
                                <Input placeholder="0.0.0.0" />
                            </Form.Item>
                            <Form.Item label="监听端口" name="port">
                                <InputNumber min={1} max={65535} className="!w-full" />
                            </Form.Item>
                        </div>
                    ) : (
                        <Form.List name="targets">
                            {(fields, { add, remove: removeTarget }) => (
                                <Space direction="vertical" className="w-full">
                                    {fields.map((field) => (
                                        <div key={field.key} className="rounded border p-3">
                                            <Form.Item name={[field.name, 'id']} hidden>
                                                <Input />
                                            </Form.Item>
                                            <div className="mb-2 flex justify-between">
                                                <span>目标 {field.name + 1}</span>
                                                <Button
                                                    type="text"
                                                    danger
                                                    icon={<MinusCircleOutlined />}
                                                    onClick={() => removeTarget(field.name)}
                                                />
                                            </div>
                                            <div className="grid grid-cols-2 gap-3">
                                                <Form.Item label="名称" name={[field.name, 'name']}>
                                                    <Input />
                                                </Form.Item>
                                                <Form.Item
                                                    label="状态"
                                                    name={[field.name, 'status']}
                                                >
                                                    <Select
                                                        options={[
                                                            { value: 'enabled', label: '启用' },
                                                            { value: 'disabled', label: '禁用' },
                                                        ]}
                                                    />
                                                </Form.Item>
                                                <Form.Item label="IP" name={[field.name, 'ip']}>
                                                    <Input />
                                                </Form.Item>
                                                <Form.Item label="端口" name={[field.name, 'port']}>
                                                    <InputNumber
                                                        min={1}
                                                        max={65535}
                                                        className="!w-full"
                                                    />
                                                </Form.Item>
                                            </div>
                                        </div>
                                    ))}
                                    <Button
                                        block
                                        type="dashed"
                                        icon={<PlusOutlined />}
                                        onClick={() => add(newTarget())}
                                    >
                                        添加目标
                                    </Button>
                                </Space>
                            )}
                        </Form.List>
                    )}
                    <Form.Item label="状态" name="status" className="mt-3">
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
