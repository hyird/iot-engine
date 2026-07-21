/**
 * SL651 字典配置 Modal
 */

import { Button, Flex, Form, Input, Select, Tooltip } from 'antd';
import { forwardRef, useImperativeHandle, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { Protocol, SL651 } from '../protocol.types';
import type { FormCondition, FormMapItem, SaveMutation } from './shared';

export interface DictConfigModalRef {
    open: (typeId: string, funcId: string, element: SL651.Element) => void;
}

interface DictConfigModalProps {
    types: Protocol.Item[];
    onSuccess?: () => void;
    saveMutation: SaveMutation;
}

const DictConfigModal = forwardRef<DictConfigModalRef, DictConfigModalProps>(
    ({ types, onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [typeId, setTypeId] = useState<string>();
        const [funcId, setFuncId] = useState<string>();
        const [element, setElement] = useState<SL651.Element>();
        const [pendingElement, setPendingElement] = useState<SL651.Element>();
        const [form] = Form.useForm();

        /** Modal 打开动画完成后设置表单值，避免闪烁 */
        const initForm = (ele: SL651.Element) => {
            form.resetFields();

            const mapType = ele.dictConfig?.mapType || 'VALUE';

            const items = (ele.dictConfig?.items || [])
                .filter((item) => item && typeof item === 'object')
                .map((item) => {
                    if (mapType === 'VALUE') {
                        return {
                            key: item.key || '',
                            label: item.label || '',
                        };
                    }
                    const validConditions = (item.dependsOn?.conditions || [])
                        .filter(
                            (c: FormCondition) =>
                                c &&
                                typeof c === 'object' &&
                                c.bitIndex !== undefined &&
                                c.bitValue !== undefined
                        )
                        .map((c: FormCondition) => ({
                            bitIndex: String(c.bitIndex),
                            bitValue: String(c.bitValue),
                        }));

                    return {
                        key: item.key || '',
                        label: item.label || '',
                        value: item.value || '1',
                        dependsOn: {
                            operator: item.dependsOn?.operator || 'AND',
                            conditions: validConditions,
                        },
                    };
                });

            form.setFieldsValue({ mapType, items });
        };

        useImperativeHandle(ref, () => ({
            open(t, fId, ele) {
                setTypeId(t);
                setFuncId(fId);
                setElement(ele);
                setPendingElement(ele);
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            if (!typeId || !funcId || !element) return;
            const values = await form.validateFields();

            const type = types.find((t) => t.id === typeId);
            if (!type) return;

            // 根据映射类型清理数据，过滤掉空项
            const cleanedItems = (values.items || [])
                .filter((item: FormMapItem) => {
                    const key = String(item.key || '').trim();
                    const label = String(item.label || '').trim();
                    return key !== '' && label !== '';
                })
                .map((item: FormMapItem) => {
                    if (values.mapType === 'VALUE') {
                        return {
                            key: String(item.key || '').trim(),
                            label: String(item.label || '').trim(),
                        };
                    } else {
                        const cleanedItem: SL651.DictMapItem = {
                            key: String(item.key || '').trim(),
                            label: String(item.label || '').trim(),
                            value: item.value || '1',
                        };

                        const validConditions = (item.dependsOn?.conditions || [])
                            .filter(
                                (c: FormCondition) =>
                                    c &&
                                    typeof c === 'object' &&
                                    c.bitIndex !== undefined &&
                                    c.bitValue !== undefined &&
                                    String(c.bitIndex).trim() !== ''
                            )
                            .map((c: FormCondition) => ({
                                bitIndex: String(c.bitIndex).trim(),
                                bitValue: String(c.bitValue),
                            }));

                        if (validConditions.length > 0) {
                            cleanedItem.dependsOn = {
                                operator: item.dependsOn?.operator || 'AND',
                                conditions: validConditions,
                            };
                        }

                        return cleanedItem;
                    }
                });

            const config = type.config as SL651.Config;
            const newFuncs = config.funcs.map((f) => {
                if (f.id !== funcId) return f;

                const newElements = f.elements.map((e) => {
                    if (e.id !== element.id) return e;

                    if (cleanedItems.length === 0) {
                        const { dictConfig: _dictConfig, ...rest } = e;
                        return rest;
                    }

                    return {
                        ...e,
                        dictConfig: {
                            mapType: values.mapType,
                            items: cleanedItems,
                        },
                    };
                });

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

        const mapType = Form.useWatch('mapType', form);

        return (
            <FormModal
                open={open}
                title={`字典配置 - ${element?.name}`}
                onCancel={() => setOpen(false)}
                onOk={handleOk}
                confirmLoading={saveMutation.isPending}
                afterOpenChange={(visible) => {
                    if (visible && pendingElement) {
                        initForm(pendingElement);
                        setPendingElement(undefined);
                    }
                }}
                forceRender
                width={800}
            >
                <Form form={form} layout="vertical">
                    <Form.Item
                        label="映射类型"
                        name="mapType"
                        rules={[{ required: true, message: '请选择映射类型' }]}
                    >
                        <Select
                            options={[
                                { value: 'VALUE', label: '值映射 - 根据数值映射文本' },
                                { value: 'BIT', label: '位映射 - 根据二进制位映射文本' },
                            ]}
                            onChange={() => {
                                form.setFieldsValue({ items: [] });
                            }}
                        />
                    </Form.Item>

                    <Form.Item
                        label="映射项"
                        extra={
                            mapType === 'BIT'
                                ? '配置二进制位对应的文本（位号范围 0-31，可选择位值为0或1时触发）'
                                : '配置数值对应的文本'
                        }
                    >
                        <Form.List name="items">
                            {(fields, { add, remove }) => (
                                <>
                                    {fields.map(({ key, name: itemName, ...restField }) => (
                                        <div
                                            key={key}
                                            className="mb-3 border border-gray-100 p-3 rounded"
                                        >
                                            {mapType === 'BIT' ? (
                                                <>
                                                    <Flex gap={8} align="center" wrap="nowrap">
                                                        <Form.Item
                                                            {...restField}
                                                            name={[itemName, 'key']}
                                                            rules={[
                                                                {
                                                                    required: true,
                                                                    message: '请输入位号',
                                                                },
                                                                {
                                                                    pattern:
                                                                        /^([0-9]|[1-2][0-9]|3[0-1])$/,
                                                                    message: '位号范围 0-31',
                                                                },
                                                            ]}
                                                            className="flex-1 !mb-0"
                                                        >
                                                            <Input placeholder="位号(0-31)" />
                                                        </Form.Item>
                                                        <Form.Item
                                                            {...restField}
                                                            name={[itemName, 'value']}
                                                            initialValue="1"
                                                            rules={[
                                                                {
                                                                    required: true,
                                                                    message: '请选择触发值',
                                                                },
                                                            ]}
                                                            className="w-20 !mb-0"
                                                        >
                                                            <Select
                                                                options={[
                                                                    { value: '1', label: '位=1' },
                                                                    { value: '0', label: '位=0' },
                                                                ]}
                                                            />
                                                        </Form.Item>
                                                        <Form.Item
                                                            {...restField}
                                                            name={[itemName, 'label']}
                                                            rules={[
                                                                { required: true, message: '' },
                                                            ]}
                                                            className="flex-[2] !mb-0"
                                                        >
                                                            <Input placeholder="映射文本" />
                                                        </Form.Item>
                                                        <Tooltip title="添加依赖条件">
                                                            <Button
                                                                size="small"
                                                                type="text"
                                                                icon={
                                                                    <span className="text-base font-bold">
                                                                        +
                                                                    </span>
                                                                }
                                                                onClick={(e) => {
                                                                    e.stopPropagation();
                                                                    const conditions =
                                                                        form.getFieldValue([
                                                                            'items',
                                                                            itemName,
                                                                            'dependsOn',
                                                                            'conditions',
                                                                        ]) || [];
                                                                    form.setFieldValue(
                                                                        [
                                                                            'items',
                                                                            itemName,
                                                                            'dependsOn',
                                                                            'conditions',
                                                                        ],
                                                                        [
                                                                            ...conditions,
                                                                            {
                                                                                bitIndex: '',
                                                                                bitValue: '1',
                                                                            },
                                                                        ]
                                                                    );
                                                                }}
                                                                className="shrink-0"
                                                            />
                                                        </Tooltip>
                                                        <Button
                                                            type="text"
                                                            danger
                                                            onClick={(e) => {
                                                                e.stopPropagation();
                                                                remove(itemName);
                                                            }}
                                                            className="shrink-0"
                                                        >
                                                            删除
                                                        </Button>
                                                    </Flex>

                                                    {/* operator 字段 */}
                                                    <Form.Item
                                                        noStyle
                                                        shouldUpdate={(prev, curr) => {
                                                            const prevConds =
                                                                prev.items?.[itemName]?.dependsOn
                                                                    ?.conditions;
                                                            const currConds =
                                                                curr.items?.[itemName]?.dependsOn
                                                                    ?.conditions;
                                                            return (
                                                                prevConds?.length !==
                                                                currConds?.length
                                                            );
                                                        }}
                                                    >
                                                        {() => {
                                                            const conditions =
                                                                form.getFieldValue([
                                                                    'items',
                                                                    itemName,
                                                                    'dependsOn',
                                                                    'conditions',
                                                                ]) || [];

                                                            return conditions.length > 0 ? (
                                                                <div className="pl-3 border-l-2 border-gray-200 mt-2">
                                                                    <Flex
                                                                        justify="space-between"
                                                                        align="center"
                                                                        className="mb-2"
                                                                    >
                                                                        <span className="text-xs text-gray-500">
                                                                            依赖条件
                                                                        </span>
                                                                        <Form.Item
                                                                            name={[
                                                                                itemName,
                                                                                'dependsOn',
                                                                                'operator',
                                                                            ]}
                                                                            initialValue="AND"
                                                                            className="!mb-0"
                                                                        >
                                                                            <Select
                                                                                size="small"
                                                                                className="!w-[140px]"
                                                                                options={[
                                                                                    {
                                                                                        value: 'AND',
                                                                                        label: 'AND（全满足）',
                                                                                    },
                                                                                    {
                                                                                        value: 'OR',
                                                                                        label: 'OR（任一满足）',
                                                                                    },
                                                                                ]}
                                                                            />
                                                                        </Form.Item>
                                                                    </Flex>
                                                                </div>
                                                            ) : null;
                                                        }}
                                                    </Form.Item>

                                                    {/* 依赖条件列表 */}
                                                    <Form.List
                                                        name={[itemName, 'dependsOn', 'conditions']}
                                                    >
                                                        {(condFields, { remove: removeCond }) => (
                                                            <>
                                                                {condFields.length > 0 && (
                                                                    <div className="pl-3 border-l-2 border-gray-200 -mt-2">
                                                                        {condFields.map(
                                                                            ({
                                                                                key: condKey,
                                                                                name: condName,
                                                                                ...condRestField
                                                                            }) => (
                                                                                <Flex
                                                                                    key={condKey}
                                                                                    gap={8}
                                                                                    align="center"
                                                                                    className="mb-1"
                                                                                >
                                                                                    <span className="text-xs text-gray-400 w-[60px]">
                                                                                        依赖位号
                                                                                    </span>
                                                                                    <Form.Item
                                                                                        {...condRestField}
                                                                                        name={[
                                                                                            condName,
                                                                                            'bitIndex',
                                                                                        ]}
                                                                                        rules={[
                                                                                            {
                                                                                                required: true,
                                                                                                message:
                                                                                                    '请输入位号',
                                                                                            },
                                                                                            {
                                                                                                pattern:
                                                                                                    /^([0-9]|[1-2][0-9]|3[0-1])$/,
                                                                                                message:
                                                                                                    '位号 0-31',
                                                                                            },
                                                                                        ]}
                                                                                        className="flex-1 !mb-0"
                                                                                    >
                                                                                        <Input
                                                                                            size="small"
                                                                                            placeholder="位号(0-31)"
                                                                                        />
                                                                                    </Form.Item>
                                                                                    <span className="text-xs text-gray-400">
                                                                                        期望值
                                                                                    </span>
                                                                                    <Form.Item
                                                                                        {...condRestField}
                                                                                        name={[
                                                                                            condName,
                                                                                            'bitValue',
                                                                                        ]}
                                                                                        initialValue="1"
                                                                                        rules={[
                                                                                            {
                                                                                                required: true,
                                                                                                message:
                                                                                                    '请选择',
                                                                                            },
                                                                                        ]}
                                                                                        className="w-20 !mb-0"
                                                                                    >
                                                                                        <Select
                                                                                            size="small"
                                                                                            options={[
                                                                                                {
                                                                                                    value: '1',
                                                                                                    label: '=1',
                                                                                                },
                                                                                                {
                                                                                                    value: '0',
                                                                                                    label: '=0',
                                                                                                },
                                                                                            ]}
                                                                                        />
                                                                                    </Form.Item>
                                                                                    <Button
                                                                                        size="small"
                                                                                        type="text"
                                                                                        danger
                                                                                        onClick={(
                                                                                            e
                                                                                        ) => {
                                                                                            e.stopPropagation();
                                                                                            removeCond(
                                                                                                condName
                                                                                            );
                                                                                        }}
                                                                                    >
                                                                                        删除
                                                                                    </Button>
                                                                                </Flex>
                                                                            )
                                                                        )}
                                                                    </div>
                                                                )}
                                                            </>
                                                        )}
                                                    </Form.List>
                                                </>
                                            ) : (
                                                <Flex gap={8} align="center">
                                                    <Form.Item
                                                        {...restField}
                                                        name={[itemName, 'key']}
                                                        rules={[
                                                            { required: true, message: '请输入值' },
                                                        ]}
                                                        className="flex-1 !mb-0"
                                                    >
                                                        <Input placeholder="数值" />
                                                    </Form.Item>
                                                    <Form.Item
                                                        {...restField}
                                                        name={[itemName, 'label']}
                                                        rules={[{ required: true, message: '' }]}
                                                        className="flex-1 !mb-0"
                                                    >
                                                        <Input placeholder="映射文本" />
                                                    </Form.Item>
                                                    <Button
                                                        type="text"
                                                        danger
                                                        onClick={() => remove(itemName)}
                                                    >
                                                        删除
                                                    </Button>
                                                </Flex>
                                            )}
                                        </div>
                                    ))}
                                    <Button
                                        type="dashed"
                                        onClick={() => {
                                            if (mapType === 'BIT') {
                                                add({
                                                    key: '',
                                                    label: '',
                                                    value: '1',
                                                    dependsOn: { operator: 'AND', conditions: [] },
                                                });
                                            } else {
                                                add({ key: '', label: '' });
                                            }
                                        }}
                                        block
                                    >
                                        + 添加映射项
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

export default DictConfigModal;
