/**
 * SL651 预设值 Modal
 */

import { Button, Flex, Form, Input } from 'antd';
import { forwardRef, useImperativeHandle, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { Protocol, SL651 } from '../protocol.types';
import type { SaveMutation } from './shared';

export interface PresetValueModalRef {
    open: (typeId: number, funcId: string, element: SL651.Element) => void;
}

interface PresetValueModalProps {
    types: Protocol.Item[];
    onSuccess?: () => void;
    saveMutation: SaveMutation;
}

const PresetValueModal = forwardRef<PresetValueModalRef, PresetValueModalProps>(
    ({ types, onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [typeId, setTypeId] = useState<number>();
        const [funcId, setFuncId] = useState<string>();
        const [element, setElement] = useState<SL651.Element>();
        const [form] = Form.useForm();

        useImperativeHandle(ref, () => ({
            open(t, fId, ele) {
                setTypeId(t);
                setFuncId(fId);
                setElement(ele);
                form.resetFields();
                form.setFieldsValue({ options: ele.options || [] });
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            if (!typeId || !funcId || !element) return;
            const values = await form.validateFields();

            const type = types.find((t) => t.id === typeId);
            if (!type) return;

            const config = type.config as SL651.Config;
            const newFuncs = config.funcs.map((f) => {
                if (f.id !== funcId) return f;

                const newElements = f.elements.map((e) =>
                    e.id === element.id ? { ...e, options: values.options } : e
                );

                return { ...f, elements: newElements };
            });

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
                open={open}
                title={`配置预设值 - ${element?.name}`}
                onCancel={() => setOpen(false)}
                onOk={handleOk}
                confirmLoading={saveMutation.isPending}
                forceRender
                width={600}
            >
                <Form form={form} layout="vertical">
                    <Form.Item label="预设值选项" extra="配置后指令下发时可从下拉列表选择">
                        <Form.List name="options">
                            {(fields, { add, remove }) => (
                                <>
                                    {fields.map(({ key, name, ...restField }) => (
                                        <Flex key={key} gap={8} align="center" className="mb-2">
                                            <Form.Item
                                                {...restField}
                                                name={[name, 'label']}
                                                rules={[{ required: true, message: '请输入名称' }]}
                                                className="flex-1 !mb-0"
                                            >
                                                <Input placeholder="显示名称" />
                                            </Form.Item>
                                            <Form.Item
                                                {...restField}
                                                name={[name, 'value']}
                                                rules={[{ required: true, message: '请输入值' }]}
                                                className="flex-1 !mb-0"
                                            >
                                                <Input placeholder="实际值" />
                                            </Form.Item>
                                            <Button type="text" danger onClick={() => remove(name)}>
                                                删除
                                            </Button>
                                        </Flex>
                                    ))}
                                    <Button type="dashed" onClick={() => add()} block>
                                        + 添加预设值
                                    </Button>
                                </>
                            )}
                        </Form.List>
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);

export default PresetValueModal;
