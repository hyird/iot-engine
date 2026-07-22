import { App, Button, Form, Input, Pagination, Result, Select, Space, Table, Tag } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import { useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { PageContainer } from '@/components/PageContainer';
import { StatusTag } from '@/components/StatusTag';
import { SUPERADMIN_ROLE_CODE } from '@/config/app';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermissions } from '@/hooks/usePermission';
import { validateForm } from '@/utils/validation';
import { createRoleSchema, updateRoleSchema } from './role.schema';
import { useRoleDelete, useRoleList, useRoleSave } from './role.service';
import type { Role } from './role.types';

const { Search, TextArea } = Input;

const permissionOptions = [
    ['system:role:query', '查询角色'],
    ['system:role:add', '新增角色'],
    ['system:role:edit', '编辑角色'],
    ['system:role:delete', '删除角色'],
    ['system:dept:query', '查询部门'],
    ['system:dept:add', '新增部门'],
    ['system:dept:edit', '编辑部门'],
    ['system:dept:delete', '删除部门'],
    ['system:user:query', '查询用户'],
    ['system:user:add', '新增用户'],
    ['system:user:edit', '编辑用户'],
    ['system:user:delete', '删除用户'],
    ['iot:link:query', '查询链路'],
    ['iot:link:add', '新增链路'],
    ['iot:link:edit', '编辑链路'],
    ['iot:link:delete', '删除链路'],
    ['iot:protocol:query', '查询协议配置'],
    ['iot:protocol:add', '新增协议配置'],
    ['iot:protocol:edit', '编辑协议配置'],
    ['iot:protocol:delete', '删除协议配置'],
    ['iot:protocol:import', '导入协议配置'],
    ['iot:protocol:export', '导出协议配置'],
    ['iot:device:query', '查询设备'],
    ['iot:device:add', '新增设备'],
    ['iot:device:edit', '编辑设备'],
    ['iot:device:delete', '删除设备'],
    ['iot:device:share', '分享设备'],
    ['iot:device:command', '下发设备命令'],
    ['iot:device-group:query', '查询设备分组'],
    ['iot:device-group:add', '新增设备分组'],
    ['iot:device-group:edit', '编辑设备分组'],
    ['iot:device-group:delete', '删除设备分组'],
    ['iot:device-group:share', '分享设备分组'],
    ['iot:open-access:query', '查询开放接入'],
    ['iot:open-access:add', '新增开放接入配置'],
    ['iot:open-access:edit', '编辑开放接入配置'],
    ['iot:open-access:delete', '删除开放接入配置'],
    ['iot:edge:query', '查询边缘节点'],
    ['iot:edge:edit', '审批边缘节点'],
    ['iot:edge:config', '配置边缘节点'],
    ['iot:edge:firmware', '上传与刷写边缘固件'],
    ['iot:edge:terminal', '访问边缘 Web 终端'],
].map(([value, label]) => ({ value, label }));

type RoleFormValues = Role.CreateDto & { id?: string };

export default function SystemRolePage() {
    const [keyword, setKeyword] = useState('');
    const [pagination, setPagination] = useState({ page: 1, pageSize: 10 });
    const [editing, setEditing] = useState<Role.Item | null>(null);
    const [open, setOpen] = useState(false);
    const [form] = Form.useForm<RoleFormValues>();
    const { modal } = App.useApp();
    const { has } = usePermissions();
    const canQuery = has('system:role:query');
    const canAdd = has('system:role:add');
    const canEdit = has('system:role:edit');
    const canDelete = has('system:role:delete');
    const { run: search } = useDebounceFn((value: string) => {
        setKeyword(value);
        setPagination((current) => ({ ...current, page: 1 }));
    }, 300);
    const { data, isLoading } = useRoleList(
        { ...pagination, keyword: keyword || undefined },
        { enabled: canQuery }
    );
    const save = useRoleSave();
    const remove = useRoleDelete();

    const showCreate = () => {
        setEditing(null);
        form.resetFields();
        form.setFieldsValue({ status: 'enabled', permissions: [] });
        setOpen(true);
    };
    const showEdit = (role: Role.Item) => {
        setEditing(role);
        form.setFieldsValue({ ...role });
        setOpen(true);
    };
    const submit = (values: RoleFormValues) => {
        if (editing) {
            const validated = validateForm(form, updateRoleSchema, values);
            if (!validated) return;
            save.mutate({ ...validated, id: editing.id } as RoleFormValues, {
                onSuccess: () => setOpen(false),
            });
            return;
        }

        const validated = validateForm(form, createRoleSchema, values);
        if (validated) save.mutate(validated, { onSuccess: () => setOpen(false) });
    };
    const confirmDelete = (role: Role.Item) => {
        modal.confirm({
            title: `确认删除角色「${role.name}」吗？`,
            content: '仍有用户使用的角色不能删除。',
            okButtonProps: { danger: true },
            onOk: () => remove.mutate(role.id),
        });
    };

    if (!canQuery) {
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询角色的权限" />
            </PageContainer>
        );
    }

    const columns: ColumnsType<Role.Item> = [
        { title: '角色名称', dataIndex: 'name' },
        { title: '角色编码', dataIndex: 'code' },
        { title: '描述', dataIndex: 'description', render: (value) => value || '-' },
        {
            title: '权限',
            dataIndex: 'permissions',
            render: (permissions: string[]) =>
                permissions.includes('*') ? (
                    <Tag color="gold">全部权限</Tag>
                ) : (
                    <Tag>{permissions.length} 项</Tag>
                ),
        },
        {
            title: '状态',
            dataIndex: 'status',
            render: (value: Role.Status) => <StatusTag status={value} />,
        },
        {
            title: '操作',
            key: 'actions',
            width: 140,
            render: (_, role) => {
                const builtin = role.code === SUPERADMIN_ROLE_CODE;
                return (
                    <Space>
                        {canEdit && (
                            <Button type="link" disabled={builtin} onClick={() => showEdit(role)}>
                                编辑
                            </Button>
                        )}
                        {canDelete && (
                            <Button
                                type="link"
                                danger
                                disabled={builtin}
                                onClick={() => confirmDelete(role)}
                            >
                                删除
                            </Button>
                        )}
                    </Space>
                );
            },
        },
    ];

    return (
        <PageContainer
            header={
                <div className="flex flex-wrap items-center justify-between gap-3">
                    <div>
                        <h2 className="m-0 text-lg font-semibold text-slate-900">角色管理</h2>
                        <p className="m-0 mt-1 text-xs text-slate-500">维护角色权限与可用状态</p>
                    </div>
                    <Space wrap>
                        <Search
                            allowClear
                            placeholder="角色名称 / 编码"
                            onChange={(event) => search(event.target.value)}
                            className="w-[240px]"
                        />
                        {canAdd && (
                            <Button type="primary" onClick={showCreate}>
                                新建角色
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
                        showTotal={(total) => `共 ${total} 条`}
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
                sticky
                scroll={{ x: 'max-content', y: 'calc(100vh - 280px)' }}
            />
            <FormModal
                open={open}
                title={editing ? '编辑角色' : '新建角色'}
                onCancel={() => setOpen(false)}
                onOk={() => form.submit()}
                confirmLoading={save.isPending}
                destroyOnHidden
            >
                <Form form={form} layout="vertical" onFinish={submit}>
                    <Form.Item name="id" hidden>
                        <Input />
                    </Form.Item>
                    <Form.Item label="角色名称" name="name">
                        <Input maxLength={128} />
                    </Form.Item>
                    <Form.Item label="角色编码" name="code">
                        <Input maxLength={64} />
                    </Form.Item>
                    <Form.Item label="描述" name="description">
                        <TextArea rows={3} maxLength={500} />
                    </Form.Item>
                    <Form.Item label="权限" name="permissions">
                        <Select mode="multiple" options={permissionOptions} />
                    </Form.Item>
                    <Form.Item label="状态" name="status">
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
