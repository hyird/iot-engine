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
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { useMemo, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { StatusTag } from '@/components/StatusTag';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { useUserOptions } from '../user/user.service';
import { useDeptDelete, useDeptList, useDeptOptions, useDeptSave } from './dept.service';
import type { Dept } from './dept.types';

const { Search } = Input;
type DeptFormValues = Dept.CreateDto & { id?: number };

export default function SystemDeptPage() {
    const [keyword, setKeyword] = useState('');
    const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
    const [editing, setEditing] = useState<Dept.Item | null>(null);
    const [open, setOpen] = useState(false);
    const [form] = Form.useForm<DeptFormValues>();
    const { modal } = App.useApp();
    const { has } = usePermissions();
    const canQuery = has('system:dept:query');
    const canAdd = has('system:dept:add');
    const canEdit = has('system:dept:edit');
    const canDelete = has('system:dept:delete');
    const { run: search } = useDebounceFn((value: string) => {
        setKeyword(value);
        setPagination((current) => ({ ...current, page: 1 }));
    }, 300);
    const { data, isLoading } = useDeptList(
        { ...pagination, keyword: keyword || undefined },
        { enabled: canQuery }
    );
    const { data: departments = [] } = useDeptOptions({ enabled: canQuery || canAdd || canEdit });
    const { data: users = [] } = useUserOptions({ enabled: canAdd || canEdit });
    const save = useDeptSave();
    const remove = useDeptDelete();
    const parentOptions = useMemo(
        () =>
            departments
                .filter((item) => item.id !== editing?.id)
                .map((item) => ({ value: item.id, label: item.name })),
        [departments, editing]
    );
    const leaderOptions = useMemo(
        () => users.map((item) => ({ value: item.id, label: item.nickname || item.username })),
        [users]
    );

    const showCreate = () => {
        setEditing(null);
        form.resetFields();
        form.setFieldsValue({ parent_id: 0, leader_id: 0, sort_order: 0, status: 'enabled' });
        setOpen(true);
    };
    const showEdit = (dept: Dept.Item) => {
        setEditing(dept);
        form.setFieldsValue({ ...dept });
        setOpen(true);
    };
    const submit = (values: DeptFormValues) =>
        save.mutate(values, { onSuccess: () => setOpen(false) });
    const confirmDelete = (dept: Dept.Item) =>
        modal.confirm({
            title: `确认删除部门「${dept.name}」吗？`,
            content: '存在子部门时不能删除。',
            okButtonProps: { danger: true },
            onOk: () => remove.mutate(dept.id),
        });

    if (!canQuery)
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询部门的权限" />
            </PageContainer>
        );

    const columns: ColumnsType<Dept.Item> = [
        { title: '部门名称', dataIndex: 'name' },
        { title: '部门编码', dataIndex: 'code', render: (value) => value || '-' },
        { title: '上级部门', dataIndex: 'parent_name', render: (value) => value || '-' },
        { title: '负责人', dataIndex: 'leader_name', render: (value) => value || '-' },
        { title: '排序', dataIndex: 'sort_order', width: 80 },
        {
            title: '状态',
            dataIndex: 'status',
            render: (value: Dept.Status) => <StatusTag status={value} />,
        },
        {
            title: '操作',
            key: 'actions',
            width: 140,
            render: (_, dept) => (
                <Space>
                    {canEdit && (
                        <Button type="link" onClick={() => showEdit(dept)}>
                            编辑
                        </Button>
                    )}
                    {canDelete && (
                        <Button type="link" danger onClick={() => confirmDelete(dept)}>
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
                    <h3 className="m-0 text-base font-medium">部门管理</h3>
                    <Space>
                        <Search
                            allowClear
                            placeholder="部门名称 / 编码"
                            onChange={(event) => search(event.target.value)}
                        />
                        {canAdd && (
                            <Button type="primary" onClick={showCreate}>
                                新建部门
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
                title={editing ? '编辑部门' : '新建部门'}
                onCancel={() => setOpen(false)}
                onOk={() => form.submit()}
                confirmLoading={save.isPending}
                destroyOnHidden
            >
                <Form form={form} layout="vertical" onFinish={submit}>
                    <Form.Item name="id" hidden>
                        <Input />
                    </Form.Item>
                    <Form.Item label="部门名称" name="name" rules={[{ required: true }]}>
                        <Input />
                    </Form.Item>
                    <Form.Item label="部门编码" name="code">
                        <Input />
                    </Form.Item>
                    <Form.Item label="上级部门" name="parent_id">
                        <Select options={[{ value: 0, label: '顶级部门' }, ...parentOptions]} />
                    </Form.Item>
                    <Form.Item label="负责人" name="leader_id">
                        <Select
                            showSearch
                            optionFilterProp="label"
                            options={[{ value: 0, label: '未指定' }, ...leaderOptions]}
                        />
                    </Form.Item>
                    <Form.Item label="排序" name="sort_order">
                        <InputNumber min={0} className="!w-full" />
                    </Form.Item>
                    <Form.Item label="状态" name="status" rules={[{ required: true }]}>
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
