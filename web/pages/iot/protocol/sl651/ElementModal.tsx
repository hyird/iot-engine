/**
 * SL651 要素 Modal
 */

import { AutoComplete, Form, Input, InputNumber, Select } from 'antd';
import { forwardRef, useImperativeHandle, useMemo, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { Protocol, SL651 } from '../protocol.types';
import { normalizeGroupName } from '../grouping';
import { useFilterableGroupOptions } from '../useFilterableGroupOptions';
import { EncodeList, generateId, type SaveMutation } from './shared';

export interface ElementModalRef {
    open: (
        mode: 'create' | 'edit',
        typeId: number,
        funcId: string,
        element?: SL651.Element
    ) => void;
}

interface ElementModalProps {
    types: Protocol.Item[];
    onSuccess?: () => void;
    saveMutation: SaveMutation;
}

const ElementModal = forwardRef<ElementModalRef, ElementModalProps>(
    ({ types, onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [mode, setMode] = useState<'create' | 'edit'>('create');
        const [typeId, setTypeId] = useState<number>();
        const [funcId, setFuncId] = useState<string>();
        const [current, setCurrent] = useState<SL651.Element>();
        const [form] = Form.useForm();
        const groupNames = useMemo(() => {
            const type = types.find((t) => t.id === typeId);
            const config = type?.config as SL651.Config | undefined;
            const groups = new Set<string>();

            for (const func of config?.funcs || []) {
                for (const element of func.elements || []) {
                    const group = normalizeGroupName(element.group);
                    if (group) groups.add(group);
                }
                for (const element of func.responseElements || []) {
                    const group = normalizeGroupName(element.group);
                    if (group) groups.add(group);
                }
            }

            const currentGroup = normalizeGroupName(current?.group);
            if (currentGroup) groups.add(currentGroup);

            return Array.from(groups);
        }, [current?.group, typeId, types]);
        const groupOptions = useFilterableGroupOptions(groupNames);

        useImperativeHandle(ref, () => ({
            open(m, t, fId, element) {
                setMode(m);
                setTypeId(t);
                setFuncId(fId);
                setCurrent(element);
                form.resetFields();
                form.setFieldsValue(element ?? { encode: 'BCD', length: 1, digits: 0 });
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            if (!typeId || !funcId) return;
            const values = await form.validateFields();

            const type = types.find((t) => t.id === typeId);
            if (!type) return;

            const config = type.config as SL651.Config;
            const elementFields = {
                name: values.name,
                group: normalizeGroupName(values.group) || undefined,
                guideHex: values.guideHex,
                encode: values.encode,
                length: values.length,
                digits: values.digits,
                unit: values.unit,
                remark: values.remark,
            };
            const newFuncs = config.funcs.map((f) => {
                if (f.id !== funcId) return f;

                let newElements: SL651.Element[];
                if (mode === 'create') {
                    const newElement: SL651.Element = {
                        id: generateId(),
                        ...elementFields,
                    };
                    newElements = [...(f.elements || []), newElement];
                } else {
                    newElements = f.elements.map((e) =>
                        e.id === current?.id ? { ...e, ...elementFields } : e
                    );
                }

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
                title={mode === 'create' ? '新增要素' : '编辑要素'}
                onCancel={() => setOpen(false)}
                onOk={handleOk}
                confirmLoading={saveMutation.isPending}
                forceRender
                width={520}
            >
                <Form form={form} layout="vertical">
                    <Form.Item
                        label="要素名称"
                        name="name"
                        rules={[{ required: true, message: '请输入名称' }]}
                    >
                        <Input />
                    </Form.Item>
                    <Form.Item
                        label="分组"
                        name="group"
                        extra="同一分组的要素会在配置页聚合为同一组卡片，留空则显示在未分组中"
                    >
                        <AutoComplete
                            allowClear
                            options={groupOptions.options}
                            placeholder="例如：基础信息、告警、控制"
                            filterOption={false}
                            onDropdownVisibleChange={groupOptions.onDropdownVisibleChange}
                            onSearch={groupOptions.onSearch}
                        />
                    </Form.Item>
                    <Form.Item
                        label="引导符（HEX）"
                        name="guideHex"
                        rules={[{ required: true, message: '请输入引导符' }]}
                    >
                        <Input placeholder="例如：01 或 F3F3" />
                    </Form.Item>
                    <Form.Item
                        label="编码"
                        name="encode"
                        rules={[{ required: true, message: '请选择编码' }]}
                    >
                        <Select options={EncodeList.map((e) => ({ value: e, label: e }))} />
                    </Form.Item>
                    <Form.Item
                        label="长度"
                        name="length"
                        rules={[{ required: true, message: '请输入长度' }]}
                    >
                        <InputNumber min={1} className="!w-full" />
                    </Form.Item>
                    <Form.Item label="单位" name="unit">
                        <Input placeholder="例如 V、℃、m³/s" />
                    </Form.Item>
                    <Form.Item
                        label="小数位数"
                        name="digits"
                        rules={[{ required: true, message: '请输入小数位数' }]}
                    >
                        <InputNumber min={0} max={8} className="!w-full" />
                    </Form.Item>
                    <Form.Item label="备注" name="remark">
                        <Input.TextArea rows={2} />
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);

export default ElementModal;
