import { Form, Input, InputNumber, Select, TreeSelect } from 'antd';
import { useEffect, useMemo } from 'react';
import { FormModal } from '@/components/FormModal';
import type { Edge } from './edge-node.types';

interface Props {
    open: boolean;
    editing: Edge.GroupTreeItem | null;
    parentId: string | null;
    treeData: Edge.GroupTreeItem[];
    loading: boolean;
    onCancel: () => void;
    onFinish: (values: Edge.GroupSaveDto & { id?: string }) => void;
}

function groupOptions(
    nodes: Edge.GroupTreeItem[],
    excludeId?: string
): { value: string; title: string; children?: ReturnType<typeof groupOptions> }[] {
    return nodes
        .filter((node) => node.id !== excludeId)
        .map((node) => ({
            value: node.id,
            title: node.name,
            children: node.children?.length
                ? groupOptions(node.children, excludeId)
                : undefined,
        }));
}

export default function EdgeNodeGroupFormModal({
    open,
    editing,
    parentId,
    treeData,
    loading,
    onCancel,
    onFinish,
}: Props) {
    const [form] = Form.useForm<Edge.GroupSaveDto & { id?: string }>();

    useEffect(() => {
        if (!open) return;
        if (editing) {
            form.setFieldsValue({
                id: editing.id,
                name: editing.name,
                parentId: editing.parentId || undefined,
                sortOrder: editing.sortOrder,
                status: editing.status,
                remark: editing.remark,
            });
            return;
        }
        form.resetFields();
        form.setFieldsValue({
            parentId: parentId || undefined,
            sortOrder: 0,
            status: 'enabled',
            remark: '',
        });
    }, [editing, form, open, parentId]);

    const treeDataForSelect = useMemo(
        () => groupOptions(treeData, editing?.id),
        [editing?.id, treeData]
    );

    return (
        <FormModal
            open={open}
            title={editing ? '编辑边缘节点分组' : '新建边缘节点分组'}
            okText="确定"
            cancelText="取消"
            confirmLoading={loading}
            onCancel={onCancel}
            onOk={() => form.submit()}
            destroyOnClose
        >
            <Form form={form} layout="vertical" onFinish={onFinish} className="mt-4">
                <Form.Item name="id" hidden>
                    <Input />
                </Form.Item>
                <Form.Item
                    label="分组名称"
                    name="name"
                    rules={[{ required: true, message: '请输入分组名称' }]}
                >
                    <Input placeholder="请输入分组名称" maxLength={100} />
                </Form.Item>
                <Form.Item label="上级分组" name="parentId">
                    <TreeSelect
                        allowClear
                        treeData={treeDataForSelect}
                        placeholder="不选则为顶级分组"
                        treeDefaultExpandAll
                    />
                </Form.Item>
                <Form.Item label="排序" name="sortOrder">
                    <InputNumber className="!w-full" min={0} placeholder="数值越小越靠前" />
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
                <Form.Item label="备注" name="remark">
                    <Input.TextArea rows={2} maxLength={500} placeholder="可选备注" />
                </Form.Item>
            </Form>
        </FormModal>
    );
}
