/**
 * SL651 应答要素 Modal
 */

import {
    AutoComplete,
    Button,
    Card,
    Empty,
    Flex,
    Form,
    Input,
    InputNumber,
    Select,
    Tag,
} from 'antd';
import { type CSSProperties, forwardRef, useImperativeHandle, useMemo, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { Protocol, SL651 } from '../protocol.types';
import { normalizeGroupName } from '../grouping';
import { useFilterableGroupOptions } from '../useFilterableGroupOptions';
import { EncodeList, generateId, type SaveMutation } from './shared';

export interface ResponseElementsModalRef {
    open: (typeId: string, func: SL651.Func) => void;
}

interface ResponseElementsModalProps {
    types: Protocol.Item[];
    onSuccess?: () => void;
    saveMutation: SaveMutation;
}

const RESPONSE_ELEMENT_CARD_GRID_STYLE: CSSProperties = {
    gridTemplateColumns: 'repeat(auto-fill, minmax(300px, 1fr))',
};

const ResponseElementsModal = forwardRef<ResponseElementsModalRef, ResponseElementsModalProps>(
    ({ types, onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [typeId, setTypeId] = useState<string>();
        const [func, setFunc] = useState<SL651.Func>();
        const [form] = Form.useForm();
        const groupNames = useMemo(() => {
            const groups = new Set<string>();
            for (const element of [...(func?.elements || []), ...(func?.responseElements || [])]) {
                const group = normalizeGroupName(element.group);
                if (group) groups.add(group);
            }
            return Array.from(groups);
        }, [func]);
        const groupOptions = useFilterableGroupOptions(groupNames);

        useImperativeHandle(ref, () => ({
            open(t, f) {
                setTypeId(t);
                setFunc(f);
                form.resetFields();
                form.setFieldsValue({
                    responseElements: f.responseElements || [],
                });
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            if (!typeId || !func) return;
            const values = await form.validateFields();

            const type = types.find((t) => t.id === typeId);
            if (!type) return;

            // 过滤掉不完整的要素
            const cleanedElements = (values.responseElements || [])
                .filter(
                    (ele: Partial<SL651.Element>) =>
                        ele.name?.trim() &&
                        ele.guideHex?.trim() &&
                        ele.encode &&
                        ele.length !== undefined
                )
                .map((ele: Partial<SL651.Element>) => ({
                    id: ele.id || generateId(),
                    name: ele.name?.trim(),
                    group: normalizeGroupName(ele.group) || undefined,
                    guideHex: ele.guideHex?.trim(),
                    encode: ele.encode,
                    length: ele.length,
                    digits: Number.isFinite(ele.digits) ? ele.digits : 0,
                    unit: ele.unit?.trim() || undefined,
                    remark: ele.remark?.trim() || undefined,
                }));

            const config = type.config as SL651.Config;
            const newFuncs = config.funcs.map((f) => {
                if (f.id !== func.id) return f;
                return {
                    ...f,
                    responseElements: cleanedElements.length > 0 ? cleanedElements : undefined,
                };
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
                title={`应答要素配置 - ${func?.name} (${func?.funcCode})`}
                onCancel={() => setOpen(false)}
                onOk={handleOk}
                confirmLoading={saveMutation.isPending}
                forceRender
                width={900}
            >
                <div className="mb-4">
                    <Tag color="blue">下行功能码</Tag>
                    <span className="text-gray-500 ml-2">
                        配置设备应答报文的解析要素。设备收到下行指令后会返回应答报文，这里定义如何解析应答报文中的数据。
                    </span>
                </div>

                <Form form={form} layout="vertical">
                    <Form.List name="responseElements">
                        {(fields, { add, remove }) => (
                            <>
                                {fields.length > 0 ? (
                                    <div
                                        className="grid gap-3"
                                        style={RESPONSE_ELEMENT_CARD_GRID_STYLE}
                                    >
                                        {fields.map((field, index) => (
                                            <Card
                                                key={field.key}
                                                size="small"
                                                hoverable
                                                className="h-full border-slate-200 shadow-[0_1px_4px_rgba(15,23,42,0.06)]"
                                                styles={{ body: { padding: 12 } }}
                                            >
                                                <Flex
                                                    justify="space-between"
                                                    gap={12}
                                                    align="start"
                                                    className="mb-3"
                                                >
                                                    <div className="min-w-0 flex-1">
                                                        <div className="text-sm font-semibold text-slate-800">
                                                            应答要素 {index + 1}
                                                        </div>
                                                        <div className="mt-0.5 text-[12px] text-slate-400">
                                                            配置名称、引导符、编码、长度、单位和小数位数
                                                        </div>
                                                    </div>
                                                    <Button
                                                        type="text"
                                                        danger
                                                        size="small"
                                                        onClick={() => remove(field.name)}
                                                    >
                                                        删除
                                                    </Button>
                                                </Flex>

                                                <div className="grid gap-3 sm:grid-cols-2">
                                                    <Form.Item
                                                        name={[field.name, 'name']}
                                                        rules={[
                                                            { required: true, message: '必填' },
                                                        ]}
                                                        className="!mb-0"
                                                    >
                                                        <Input placeholder="要素名称" />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'group']}
                                                        className="!mb-0"
                                                    >
                                                        <AutoComplete
                                                            allowClear
                                                            options={groupOptions.options}
                                                            placeholder="分组"
                                                            filterOption={false}
                                                            onDropdownVisibleChange={
                                                                groupOptions.onDropdownVisibleChange
                                                            }
                                                            onSearch={groupOptions.onSearch}
                                                        />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'guideHex']}
                                                        rules={[
                                                            { required: true, message: '必填' },
                                                        ]}
                                                        className="!mb-0"
                                                    >
                                                        <Input placeholder="如: 01" />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'encode']}
                                                        rules={[
                                                            { required: true, message: '必填' },
                                                        ]}
                                                        className="!mb-0"
                                                    >
                                                        <Select
                                                            placeholder="选择"
                                                            options={EncodeList.map((e) => ({
                                                                value: e,
                                                                label: e,
                                                            }))}
                                                        />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'length']}
                                                        rules={[
                                                            { required: true, message: '必填' },
                                                        ]}
                                                        className="!mb-0"
                                                    >
                                                        <InputNumber min={1} className="!w-full" />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'unit']}
                                                        className="!mb-0"
                                                    >
                                                        <Input placeholder="单位" />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'digits']}
                                                        className="!mb-0"
                                                    >
                                                        <InputNumber
                                                            min={0}
                                                            max={8}
                                                            className="!w-full"
                                                        />
                                                    </Form.Item>
                                                    <Form.Item
                                                        name={[field.name, 'remark']}
                                                        className="!mb-0 sm:col-span-2"
                                                    >
                                                        <Input placeholder="备注" />
                                                    </Form.Item>
                                                </div>
                                            </Card>
                                        ))}
                                    </div>
                                ) : (
                                    <Empty description="暂无应答要素，点击下方按钮添加" />
                                )}
                                <Button
                                    type="dashed"
                                    onClick={() =>
                                        add({
                                            id: generateId(),
                                            name: '',
                                            group: undefined,
                                            guideHex: '',
                                            encode: 'BCD',
                                            length: 1,
                                            digits: 0,
                                        })
                                    }
                                    block
                                    className="mt-3"
                                >
                                    + 添加应答要素
                                </Button>
                            </>
                        )}
                    </Form.List>
                </Form>
            </FormModal>
        );
    }
);

export default ResponseElementsModal;
