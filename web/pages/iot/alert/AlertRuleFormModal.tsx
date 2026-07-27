import { PlusOutlined } from '@ant-design/icons';
import { type UseMutationResult, useQueryClient } from '@tanstack/react-query';
import { Button, Form, Input, InputNumber, Modal, Select, Spin } from 'antd';
import { useEffect, useMemo, useState } from 'react';
import { useDeviceList } from '../device/device.service';
import { useProtocolConfigDetail } from '../protocol/protocol.service';
import type { Modbus, Protocol, S7, SL651 } from '../protocol/protocol.types';
import { alertKeys } from './alert.service';
import type { Alert } from './alert.types';
import { ConditionEditor } from './ConditionEditor';

const PROTOCOL_TYPE_OPTIONS = [
    { label: 'SL651', value: 'SL651' },
    { label: 'Modbus', value: 'Modbus' },
    { label: 'S7', value: 'S7' },
];

interface AlertRuleFormValues {
    id?: string;
    name: string;
    device_id: string;
    severity: Alert.Severity;
    conditions: Alert.Condition[];
    logic: 'and' | 'or';
    silence_duration: number;
    recovery_condition: string;
    recovery_wait_seconds: number;
    status: 'enabled' | 'disabled';
    remark?: string;
}

type EditableCondition = Alert.Condition & { _key: string };

const editableCondition = (condition: Alert.Condition): EditableCondition => ({
    ...condition,
    _key: crypto.randomUUID(),
});

interface AlertRuleFormModalProps {
    open: boolean;
    editing: Alert.RuleItem | null;
    saveMutation: UseMutationResult<void, Error, Alert.RuleDto & { id?: string }, unknown>;
    onClose: () => void;
}

export function AlertRuleFormModal({
    open,
    editing,
    saveMutation,
    onClose,
}: AlertRuleFormModalProps) {
    const [form] = Form.useForm<AlertRuleFormValues>();
    const [conditions, setConditions] = useState<EditableCondition[]>([]);
    const [protocolType, setProtocolType] = useState<Protocol.Type | ''>('');
    const queryClient = useQueryClient();

    // 获取带协议类型的设备列表（useMemo 稳定引用，避免 ?? [] 每次创建新数组）
    const { data: deviceStaticData } = useDeviceList({ enabled: open });
    const allDevices = useMemo(() => deviceStaticData?.list ?? [], [deviceStaticData?.list]);

    // 按协议类型过滤设备选项
    const filteredDevices = useMemo(
        () =>
            protocolType ? allDevices.filter((d) => d.protocol_type === protocolType) : allDevices,
        [allDevices, protocolType]
    );

    // 监听选中的设备 ID
    const selectedDeviceId = Form.useWatch('device_id', form);

    // 从设备静态数据中获取协议配置 ID
    const selectedDevice = allDevices.find((d) => d.id === selectedDeviceId);
    const protocolConfigId = selectedDevice?.protocol_config_id;

    const { data: protocolConfig, isLoading: isLoadingConfig } = useProtocolConfigDetail(
        protocolConfigId,
        { enabled: open && Boolean(protocolConfigId) }
    );

    // 从协议配置中提取要素选项
    const elementOptions = useMemo(() => {
        if (!protocolConfig?.config) return [];

        const type = selectedDevice?.protocol_type || protocolConfig.protocol;

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
    }, [protocolConfig, selectedDevice?.protocol_type]);

    useEffect(() => {
        if (open) {
            if (editing) {
                // 编辑模式：从已有设备推导协议类型
                const editDevice = allDevices.find((d) => d.id === editing.device_id);
                setProtocolType((editDevice?.protocol_type as Protocol.Type) || '');
                form.setFieldsValue({
                    id: editing.id,
                    name: editing.name,
                    device_id: editing.device_id,
                    severity: editing.severity,
                    logic: editing.logic,
                    silence_duration: editing.silence_duration,
                    recovery_condition: editing.recovery_condition,
                    recovery_wait_seconds: editing.recovery_wait_seconds,
                    status: editing.status,
                    remark: editing.remark,
                });
                setConditions((editing.conditions || []).map(editableCondition));
            } else {
                form.resetFields();
                form.setFieldsValue({
                    severity: 'warning',
                    logic: 'and',
                    silence_duration: 300,
                    recovery_condition: 'reverse',
                    recovery_wait_seconds: 60,
                    status: 'enabled',
                });
                setConditions([]);
                setProtocolType('');
            }
        }
    }, [open, editing, form, allDevices]);

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

    const onFinish = (values: AlertRuleFormValues) => {
        saveMutation.mutate(
            {
                ...values,
                conditions: conditions.map(({ _key: _unused, ...condition }) => condition),
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
            title={editing ? '编辑规则' : '新建规则'}
            onCancel={onClose}
            onOk={() => form.submit()}
            confirmLoading={saveMutation.isPending}
            destroyOnHidden
            width={720}
        >
            <Form<AlertRuleFormValues> form={form} layout="vertical" onFinish={onFinish}>
                <Form.Item name="id" hidden>
                    <Input />
                </Form.Item>

                <Form.Item
                    label="规则名称"
                    name="name"
                    rules={[{ required: true, message: '请输入规则名称' }]}
                >
                    <Input placeholder="如：水位超高告警" />
                </Form.Item>

                <div className="flex gap-4">
                    <Form.Item label="协议类型" className="w-[140px]">
                        <Select
                            value={protocolType || undefined}
                            onChange={(v) => {
                                setProtocolType(v);
                                form.setFieldValue('device_id', undefined);
                            }}
                            allowClear
                            placeholder="全部协议"
                            options={PROTOCOL_TYPE_OPTIONS}
                        />
                    </Form.Item>

                    <Form.Item
                        label="关联设备"
                        name="device_id"
                        rules={[{ required: true, message: '请选择关联设备' }]}
                        className="flex-1"
                    >
                        <Select
                            showSearch
                            optionFilterProp="label"
                            placeholder="搜索并选择设备"
                            options={filteredDevices.map((d) => ({ label: d.name, value: d.id }))}
                        />
                    </Form.Item>
                </div>

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
                            {isLoadingConfig && selectedDeviceId && (
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
                            暂无条件，请点击"添加条件"
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

                    <Form.Item label="状态" name="status" className="flex-1">
                        <Select>
                            <Select.Option value="enabled">启用</Select.Option>
                            <Select.Option value="disabled">禁用</Select.Option>
                        </Select>
                    </Form.Item>
                </div>

                <div className="flex gap-4">
                    <Form.Item label="恢复策略" name="recovery_condition" className="flex-1">
                        <Select>
                            <Select.Option value="reverse">条件反向恢复</Select.Option>
                            <Select.Option value="auto_60">自动恢复 (60秒)</Select.Option>
                            <Select.Option value="auto_300">自动恢复 (5分钟)</Select.Option>
                            <Select.Option value="auto_900">自动恢复 (15分钟)</Select.Option>
                            <Select.Option value="auto_3600">自动恢复 (1小时)</Select.Option>
                        </Select>
                    </Form.Item>

                    <Form.Item label="恢复等待(秒)" name="recovery_wait_seconds" className="flex-1">
                        <InputNumber min={0} max={86400} className="w-full" />
                    </Form.Item>
                </div>

                <Form.Item label="备注" name="remark">
                    <Input.TextArea rows={2} placeholder="可选备注" />
                </Form.Item>
            </Form>
        </Modal>
    );
}
