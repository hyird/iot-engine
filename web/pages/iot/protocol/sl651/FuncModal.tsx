/**
 * SL651 功能码 Modal
 */

import { App, Form, Input, Select } from 'antd';
import { forwardRef, useImperativeHandle, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { Protocol, SL651 } from '../protocol.types';
import { generateId, type SaveMutation } from './shared';

export interface FuncModalRef {
    open: (mode: 'create' | 'edit', typeId: number, func?: SL651.Func) => void;
}

interface FuncModalProps {
    types: Protocol.Item[];
    onSuccess?: () => void;
    saveMutation: SaveMutation;
}

const FuncModal = forwardRef<FuncModalRef, FuncModalProps>(
    ({ types, onSuccess, saveMutation }, ref) => {
        const { message } = App.useApp();
        const [open, setOpen] = useState(false);
        const [mode, setMode] = useState<'create' | 'edit'>('create');
        const [typeId, setTypeId] = useState<number>();
        const [current, setCurrent] = useState<SL651.Func>();
        const [form] = Form.useForm();

        useImperativeHandle(ref, () => ({
            open(m, t, func) {
                setMode(m);
                setTypeId(t);
                setCurrent(func);
                form.resetFields();
                form.setFieldsValue(func ?? { dir: 'UP' });
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            if (!typeId) return;
            const values = await form.validateFields();

            const type = types.find((t) => t.id === typeId);
            if (!type) return;

            const config = type.config as SL651.Config;

            // 检查功能码唯一性（排除自身）
            const duplicate = (config.funcs || []).find(
                (f) => f.funcCode === values.funcCode && (mode === 'create' || f.id !== current?.id)
            );
            if (duplicate) {
                message.error(`功能码 ${values.funcCode} 已存在（${duplicate.name}）`);
                return;
            }

            let newFuncs: SL651.Func[];

            if (mode === 'create') {
                const newFunc: SL651.Func = {
                    id: generateId(),
                    funcCode: values.funcCode,
                    dir: values.dir,
                    name: values.name,
                    remark: values.remark,
                    elements: [],
                };
                newFuncs = [...(config.funcs || []), newFunc];
            } else {
                newFuncs = config.funcs.map((f) =>
                    f.id === current?.id
                        ? {
                              ...f,
                              funcCode: values.funcCode,
                              dir: values.dir,
                              name: values.name,
                              remark: values.remark,
                          }
                        : f
                );
            }

            await saveMutation.mutateAsync({
                id: typeId,
                protocol: 'SL651',
                config: { funcs: newFuncs },
            });

            onSuccess?.();
            setOpen(false);
        };

        return (
            <FormModal
                title={mode === 'create' ? '新增功能码' : '编辑功能码'}
                open={open}
                onOk={handleOk}
                onCancel={() => setOpen(false)}
                confirmLoading={saveMutation.isPending}
                forceRender
            >
                <Form form={form} layout="vertical">
                    <Form.Item
                        label="名称"
                        name="name"
                        rules={[{ required: true, message: '请输入名称' }]}
                    >
                        <Input />
                    </Form.Item>
                    <Form.Item
                        label="功能码"
                        name="funcCode"
                        rules={[{ required: true, message: '请输入功能码' }]}
                    >
                        <Input placeholder="例如 2F" />
                    </Form.Item>
                    <Form.Item
                        label="方向"
                        name="dir"
                        rules={[{ required: true, message: '请选择方向' }]}
                    >
                        <Select
                            options={[
                                { value: 'UP', label: '上行' },
                                { value: 'DOWN', label: '下行' },
                            ]}
                        />
                    </Form.Item>
                    <Form.Item label="备注" name="remark">
                        <Input.TextArea rows={3} />
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);

export default FuncModal;
