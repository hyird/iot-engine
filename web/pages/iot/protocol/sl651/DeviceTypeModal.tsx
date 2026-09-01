/**
 * SL651 设备类型 Modal
 */

import { Divider, Form, Input, Select, Switch } from 'antd';
import { forwardRef, useEffect, useImperativeHandle, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import { STORAGE_POLICY_OPTIONS, type Protocol, type SL651 } from '../protocol.types';
import { getDeviceTypeFormValues, type SaveMutation } from './shared';

export interface DeviceTypeModalRef {
    open: (mode: 'create' | 'edit', data?: Protocol.Item) => void;
}

interface DeviceTypeModalProps {
    onSuccess?: () => void;
    saveMutation: SaveMutation;
}

const DeviceTypeModal = forwardRef<DeviceTypeModalRef, DeviceTypeModalProps>(
    ({ onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [mode, setMode] = useState<'create' | 'edit'>('create');
        const [current, setCurrent] = useState<Protocol.Item>();
        const [form] = Form.useForm();

        useImperativeHandle(ref, () => ({
            open(m, data) {
                setMode(m);
                setCurrent(data);
                setOpen(true);
            },
        }));

        useEffect(() => {
            if (!open) return;
            form.resetFields();
            form.setFieldsValue(getDeviceTypeFormValues(current));
        }, [current, form, open]);

        const handleOk = async () => {
            const values = await form.validateFields();
            const existingConfig = (current?.config as SL651.Config) || { funcs: [] };

            await saveMutation.mutateAsync({
                id: current?.id,
                protocol: 'SL651',
                name: values.name,
                enabled: values.enabled,
                config: {
                    ...existingConfig,
                    responseMode: values.responseMode,
                    storagePolicy: values.storagePolicy,
                },
                remark: values.remark,
            });

            onSuccess?.();
            setOpen(false);
        };

        return (
            <FormModal
                title={mode === 'create' ? '新增设备类型' : '编辑设备类型'}
                open={open}
                onOk={handleOk}
                onCancel={() => setOpen(false)}
                confirmLoading={saveMutation.isPending}
                forceRender
            >
                <Form form={form} layout="vertical" initialValues={getDeviceTypeFormValues()}>
                    <Divider titlePlacement="start" plain className="!my-4">
                        基础信息
                    </Divider>
                    <Form.Item
                        label="名称"
                        name="name"
                        rules={[{ required: true, message: '请输入名称' }]}
                    >
                        <Input maxLength={64} />
                    </Form.Item>
                    <Divider titlePlacement="start" plain className="!my-4">
                        协议参数
                    </Divider>
                    <Form.Item
                        label="应答模式"
                        name="responseMode"
                        rules={[{ required: true, message: '请选择应答模式' }]}
                    >
                        <Select
                            options={[
                                { value: 'M1', label: 'M1 - 自报' },
                                { value: 'M2', label: 'M2 - 自报/查询应答兼容' },
                                { value: 'M3', label: 'M3 - 查询应答' },
                                { value: 'M4', label: 'M4 - 调试/召测' },
                            ]}
                        />
                    </Form.Item>
                    <Divider titlePlacement="start" plain className="!my-4">
                        存储策略
                    </Divider>
                    <Form.Item
                        label="存储策略"
                        name="storagePolicy"
                        rules={[{ required: true, message: '请选择存储策略' }]}
                        extra="上报时存储每条历史数据；数据改变时仅在点位值变化时存储"
                    >
                        <Select options={STORAGE_POLICY_OPTIONS} />
                    </Form.Item>
                    <Divider titlePlacement="start" plain className="!my-4">
                        其他
                    </Divider>
                    <Form.Item label="启用" name="enabled" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                    <Form.Item label="备注" name="remark">
                        <Input.TextArea rows={3} />
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);

export default DeviceTypeModal;
