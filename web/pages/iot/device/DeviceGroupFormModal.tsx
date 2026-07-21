import { Form, Input, InputNumber, Select, TreeSelect } from 'antd';
import { useEffect, useMemo } from 'react';
import { FormModal } from '@/components/FormModal';
import type { DeviceGroup } from './device-group.types';

interface DeviceGroupFormModalProps {
    open: boolean;
    editing: DeviceGroup.TreeItem | null;
    parentId: string | null;
    treeData: DeviceGroup.TreeItem[];
    loading: boolean;
    onCancel: () => void;
    onFinish: (values: DeviceGroup.CreateDto & { id?: string }) => void;
}

function convertTreeForSelect(
    nodes: DeviceGroup.TreeItem[],
    excludeId?: string
): { value: string; title: string; children?: ReturnType<typeof convertTreeForSelect> }[] {
    return nodes
        .filter((n) => n.id !== excludeId)
        .map((n) => ({
            value: n.id,
            title: n.name,
            children: n.children ? convertTreeForSelect(n.children, excludeId) : undefined,
        }));
}

const DeviceGroupFormModal = ({
    open,
    editing,
    parentId,
    treeData,
    loading,
    onCancel,
    onFinish,
}: DeviceGroupFormModalProps) => {
    const [form] = Form.useForm();

    useEffect(() => {
        if (open) {
            if (editing) {
                form.setFieldsValue({
                    id: editing.id,
                    name: editing.name,
                    parent_id: editing.parent_id || undefined,
                    sort_order: editing.sort_order,
                    status: editing.status,
                    remark: editing.remark,
                });
            } else {
                form.resetFields();
                form.setFieldsValue({
                    status: 'enabled',
                    sort_order: 0,
                    parent_id: parentId || undefined,
                });
            }
        }
    }, [open, editing, parentId, form]);

    const treeSelectData = useMemo(
        () => convertTreeForSelect(treeData, editing?.id),
        [treeData, editing?.id]
    );

    return (
        <FormModal
            open={open}
            title={editing ? '编辑分组' : '新建分组'}
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
                <Form.Item label="上级分组" name="parent_id">
                    <TreeSelect
                        allowClear
                        treeData={treeSelectData}
                        placeholder="不选则为顶级分组"
                        treeDefaultExpandAll
                        fieldNames={{ label: 'title', value: 'value' }}
                    />
                </Form.Item>
                <Form.Item label="排序" name="sort_order">
                    <InputNumber className="!w-full" placeholder="数值越小越靠前" min={0} />
                </Form.Item>
                <Form.Item
                    label="状态"
                    name="status"
                    rules={[{ required: true, message: '请选择状态' }]}
                >
                    <Select>
                        <Select.Option value="enabled">启用</Select.Option>
                        <Select.Option value="disabled">禁用</Select.Option>
                    </Select>
                </Form.Item>
                <Form.Item label="备注" name="remark">
                    <Input.TextArea rows={2} placeholder="可选备注" />
                </Form.Item>
            </Form>
        </FormModal>
    );
};

export default DeviceGroupFormModal;
