import { PlusOutlined } from '@ant-design/icons';
import { type UseMutationResult, useQueryClient } from '@tanstack/react-query';
import { AutoComplete, Button, Form, Input, InputNumber, Modal, Select, Spin } from 'antd';
import { useEffect, useMemo, useState } from 'react';
import { useProtocolConfigDetail, useProtocolConfigOptions } from '../protocol/protocol.service';
import type { Modbus, Protocol, S7, SL651 } from '../protocol/protocol.types';
import { alertKeys } from './alert.service';
import type { Alert } from './alert.types';
import { ConditionEditor } from './ConditionEditor';

interface TemplateFormValues {
    id?: string;
    name: string;
    category?: string;
    description?: string;
    severity: Alert.Severity;
    logic: 'and' | 'or';
    silence_duration: number;
    recovery_condition: string;
    recovery_wait_seconds: number;
    protocol_config_id?: string;
}

type EditableCondition = Alert.Condition & { _key: string };

const editableCondition = (condition: Alert.Condition): EditableCondition => ({
    ...condition,
    _key: crypto.randomUUID(),
});

interface AlertTemplateFormModalProps {
    open: boolean;
    editing: Alert.TemplateItem | null;
    editingDetail?: Alert.TemplateDetail | null;
    saveMutation: UseMutationResult<void, Error, Alert.TemplateDto & { id?: string }, unknown>;
    onClose: () => void;
}

const PROTOCOL_TYPE_OPTIONS = [
    { label: 'SL651', value: 'SL651' },
    { label: 'Modbus', value: 'Modbus' },
    { label: 'S7', value: 'S7' },
];

export function AlertTemplateFormModal({
    open,
    editing,
    editingDetail,
    saveMutation,
    onClose,
}: AlertTemplateFormModalProps) {
    const [form] = Form.useForm<TemplateFormValues>();
    const [conditions, setConditions] = useState<EditableCondition[]>([]);
    const [protocolType, setProtocolType] = useState<Protocol.Type | ''>('');
    const queryClient = useQueryClient();

    // 根据协议类型获取配置选项列表
    const { data: configOptionsData } = useProtocolConfigOptions(protocolType as Protocol.Type, {
        enabled: open && !!protocolType,
    });
    const configOptions = configOptionsData?.list ?? [];

    // 监听表单中选中的协议配置 ID
    const selectedConfigId = Form.useWatch('protocol_config_id', form);

    // 获取协议配置详情以提取要素
    const { data: protocolConfig, isLoading: isLoadingConfig } = useProtocolConfigDetail(
        selectedConfigId,
        { enabled: open && !!selectedConfigId }
    );

    // 从协议配置中提取要素选项
    const elementOptions = useMemo(() => {
        if (!protocolConfig?.config) return [];

        const type = protocolType || protocolConfig.protocol;

        if (type === 'SL651') {
            const config = protocolConfig.config as SL651.Config;
            return (config.funcs ?? []).flatMap((f) =>
                (f.elements ?? []).map((e) => {
                    const dataKey = e.id;
                    const opt: {
                        value: string;
                        label: string;
                        dictMapType?: 'VALUE' | 'BIT';
                        dictItems?: { key: string; label: string; value?: string }[];
                    } = {
                        value: dataKey,
                        label: e.name,
                    };
                    if (e.encode === 'DICT' && e.dictConfig) {
                        opt.dictMapType = e.dictConfig.mapType;
                        opt.dictItems = e.dictConfig.items.map((d) => ({
                            key: d.key,
                            label: d.label,
                            value: d.value,
                        }));
                    }
                    return opt;
                })
            );
        }

        if (type === 'Modbus') {
            const config = protocolConfig.config as Modbus.Config;
            return (config.registers ?? []).map((r) => {
                const dataKey = r.id;
                const opt: {
                    value: string;
                    label: string;
                    dictMapType?: 'VALUE' | 'BIT';
                    dictItems?: { key: string; label: string; value?: string }[];
                } = {
                    value: dataKey,
                    label: r.name,
                };
                if (r.dictConfig?.items?.length) {
                    opt.dictMapType = 'VALUE';
                    opt.dictItems = r.dictConfig.items.map((d) => ({
                        key: d.key,
                        label: d.label,
                    }));
                }
                return opt;
            });
        }

        if (type === 'S7') {
            const config = protocolConfig.config as S7.Config;
            return (config.areas ?? []).map((area) => ({
                value: area.id,
                label: area.name,
            }));
        }

        return [];
    }, [protocolConfig, protocolType]);

    useEffect(() => {
        if (open) {
            if (editing && editingDetail) {
                // 编辑模式：从详情中恢复协议类型
                if (editingDetail.protocol_config_id) {
                    // 协议类型将在 protocolConfig 加载后自动确定
                    // 先尝试从 TemplateItem 的 protocol_type 获取
                    if (editing.protocol_type) {
                        setProtocolType(editing.protocol_type as Protocol.Type);
                    }
                } else {
                    setProtocolType('');
                }

                form.setFieldsValue({
                    id: editingDetail.id,
                    name: editingDetail.name,
                    category: editingDetail.category,
                    description: editingDetail.description,
                    severity: editingDetail.severity,
                    logic: editingDetail.logic,
                    silence_duration: editingDetail.silence_duration,
                    recovery_condition: editingDetail.recovery_condition,
                    recovery_wait_seconds: editingDetail.recovery_wait_seconds,
                    protocol_config_id: editingDetail.protocol_config_id || undefined,
                });
                setConditions((editingDetail.conditions || []).map(editableCondition));
            } else {
                form.resetFields();
                form.setFieldsValue({
                    severity: 'warning',
                    logic: 'and',
                    silence_duration: 300,
                    recovery_condition: 'reverse',
                    recovery_wait_seconds: 60,
                });
                setConditions([]);
                setProtocolType('');
            }
        }
    }, [open, editing, editingDetail, form]);

    const addCondition = () => {
        setConditions((prev) => [
            ...prev,
            editableCondition({
                type: 'threshold',
                elementKey: '',
                operator: '>',
                value: '',
            }),
        ]);
    };

    const updateCondition = (index: number, value: Alert.Condition) => {
        setConditions((prev) =>
            prev.map((condition, current) =>
                current === index ? { ...value, _key: condition._key } : condition
            )
        );
    };

    const removeCondition = (index: number) => {
        setConditions((prev) => prev.filter((_, i) => i !== index));
    };

    const onFinish = (values: TemplateFormValues) => {
        // 从选中的协议配置推导 applicable_protocols
        const applicableProtocols = protocolType ? [protocolType] : [];
        saveMutation.mutate(
            {
                ...values,
                conditions: conditions.map(({ _key: _unused, ...condition }) => condition),
                applicable_protocols: applicableProtocols,
            },
            {
                onSuccess: () => {
                    onClose();
                    queryClient.invalidateQueries({ queryKey: alertKeys.all });
                },
            }
        );
    };

    return (
        <Modal
            open={open}
            title={editing ? '编辑模板' : '新建模板'}
            onCancel={onClose}
            onOk={() => form.submit()}
            confirmLoading={saveMutation.isPending}
            destroyOnHidden
            width={720}
        >
            <Form<TemplateFormValues> form={form} layout="vertical" onFinish={onFinish}>
                <Form.Item name="id" hidden>
                    <Input />
                </Form.Item>

                <Form.Item
                    label="模板名称"
                    name="name"
                    rules={[{ required: true, message: '请输入模板名称' }]}
                >
                    <Input placeholder="如：高温告警模板" />
                </Form.Item>

                <div className="flex gap-4">
                    <Form.Item label="分类" name="category" className="flex-1">
                        <AutoComplete
                            options={['温度', '水位', '流量', '压力', '湿度', '通用'].map((v) => ({
                                value: v,
                            }))}
                            placeholder="选择或输入分类"
                            filterOption
                        />
                    </Form.Item>

                    <Form.Item label="协议类型" className="flex-1">
                        <Select
                            value={protocolType || undefined}
                            onChange={(v) => {
                                setProtocolType(v);
                                form.setFieldValue('protocol_config_id', undefined);
                            }}
                            allowClear
                            placeholder="选择协议"
                            options={PROTOCOL_TYPE_OPTIONS}
                        />
                    </Form.Item>
                </div>

                <Form.Item
                    label="设备类型（协议配置）"
                    name="protocol_config_id"
                    rules={[{ required: true, message: '请选择设备类型' }]}
                >
                    <Select
                        showSearch
                        optionFilterProp="label"
                        placeholder={protocolType ? '选择设备类型' : '请先选择协议类型'}
                        disabled={!protocolType}
                        options={configOptions.map((c) => ({ label: c.name, value: c.id }))}
                    />
                </Form.Item>

                <Form.Item label="描述" name="description">
                    <Input.TextArea rows={2} placeholder="模板描述（可选）" />
                </Form.Item>

                <div className="flex gap-4">
                    <Form.Item label="严重级别" name="severity" className="flex-1">
                        <Select>
                            <Select.Option value="critical">严重</Select.Option>
                            <Select.Option value="warning">警告</Select.Option>
                            <Select.Option value="info">信息</Select.Option>
                        </Select>
                    </Form.Item>

                    <Form.Item label="条件逻辑" name="logic" className="flex-1">
                        <Select>
                            <Select.Option value="and">全部满足 (AND)</Select.Option>
                            <Select.Option value="or">任一满足 (OR)</Select.Option>
                        </Select>
                    </Form.Item>
                </div>

                <div className="mb-4">
                    <div className="flex items-center justify-between mb-2">
                        <span className="font-medium">
                            告警条件
                            {isLoadingConfig && selectedConfigId && (
                                <Spin size="small" className="ml-2" />
                            )}
                        </span>
                        <Button
                            type="dashed"
                            size="small"
                            icon={<PlusOutlined />}
                            onClick={addCondition}
                        >
                            添加条件
                        </Button>
                    </div>
                    {conditions.length === 0 && (
                        <div className="text-gray-400 text-center py-4 border border-dashed rounded">
                            {selectedConfigId
                                ? '暂无条件，请点击"添加条件"'
                                : '请先选择设备类型，再添加条件'}
                        </div>
                    )}
                    {conditions.map((cond, index) => (
                        <ConditionEditor
                            key={cond._key}
                            value={cond}
                            elementOptions={elementOptions}
                            onChange={(v) => updateCondition(index, v)}
                            onRemove={() => removeCondition(index)}
                        />
                    ))}
                </div>

                <div className="flex gap-4">
                    <Form.Item label="冷却时间(秒)" name="silence_duration" className="flex-1">
                        <InputNumber min={0} max={86400} className="w-full" />
                    </Form.Item>

                    <Form.Item label="恢复策略" name="recovery_condition" className="flex-1">
                        <Select>
                            <Select.Option value="reverse">条件反向恢复</Select.Option>
                            <Select.Option value="auto_60">自动恢复 (60秒)</Select.Option>
                            <Select.Option value="auto_300">自动恢复 (5分钟)</Select.Option>
                            <Select.Option value="auto_900">自动恢复 (15分钟)</Select.Option>
                            <Select.Option value="auto_3600">自动恢复 (1小时)</Select.Option>
                        </Select>
                    </Form.Item>
                </div>

                <Form.Item label="恢复等待(秒)" name="recovery_wait_seconds">
                    <InputNumber min={0} max={86400} className="w-full" />
                </Form.Item>
            </Form>
        </Modal>
    );
}
