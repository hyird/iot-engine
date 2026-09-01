/**
 * Modbus 设备类型编辑弹窗（从 ModbusConfig 抽离）
 */

import { Divider, Flex, Form, Input, InputNumber, Select, Space, Switch } from 'antd';
import { forwardRef, useEffect, useImperativeHandle, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { useProtocolConfigSave } from '../protocol.service';
import { STORAGE_POLICY_OPTIONS, type Modbus, type Protocol } from '../protocol.types';
import {
    ByteOrderOptions,
    type DeviceTypeModalRef,
    getDeviceTypeFormValues,
    normalizeModbusRegisters,
    normalizePacketConfig,
    numericInputClassName,
    numericUnitClassName,
    pairedFormItemClassName,
} from './helpers';

export interface DeviceTypeModalProps {
    onSuccess?: () => void;
    saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

export const DeviceTypeModal = forwardRef<DeviceTypeModalRef, DeviceTypeModalProps>(
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
            const existingConfig = (current?.config as Modbus.Config) || { registers: [] };
            const packet = normalizePacketConfig({
                mergeGap: values.packetMergeGap,
                maxQuantity: values.packetMaxQuantity,
            });

            await saveMutation.mutateAsync({
                id: current?.id,
                protocol: 'Modbus',
                name: values.name,
                enabled: values.enabled,
                config: {
                    byteOrder: values.byteOrder,
                    readInterval: values.readInterval,
                    storagePolicy: values.storagePolicy,
                    commandFastReadDuration: values.commandFastReadDuration,
                    commandFastReadInterval: values.commandFastReadInterval,
                    packet,
                    registers: normalizeModbusRegisters(existingConfig.registers),
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
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="名称"
                            name="name"
                            rules={[{ required: true, message: '请输入名称' }]}
                            className={pairedFormItemClassName}
                        >
                            <Input placeholder="如：温湿度传感器、电表" maxLength={64} />
                        </Form.Item>
                        <Form.Item
                            label="字节序"
                            name="byteOrder"
                            rules={[{ required: true, message: '请选择字节序' }]}
                            extra="不同字节序将影响寄存器值解析"
                            className={pairedFormItemClassName}
                        >
                            <Select options={ByteOrderOptions} />
                        </Form.Item>
                    </Flex>
                    <Divider titlePlacement="start" plain className="!my-4">
                        采集与存储
                    </Divider>
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="读取间隔（秒）"
                            extra="数值越小采集越频繁，建议按设备负载设置间隔"
                            className={pairedFormItemClassName}
                        >
                            <Space.Compact block>
                                <Form.Item name="readInterval" noStyle>
                                    <InputNumber
                                        min={1}
                                        max={3600}
                                        className={numericInputClassName}
                                    />
                                </Form.Item>
                                <Input
                                    value="秒"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：秒"
                                />
                            </Space.Compact>
                        </Form.Item>
                        <Form.Item
                            label="存储策略"
                            name="storagePolicy"
                            rules={[{ required: true, message: '请选择存储策略' }]}
                            extra="上报时存储每条历史数据；数据改变时仅在点位值变化时存储"
                            className={pairedFormItemClassName}
                        >
                            <Select options={STORAGE_POLICY_OPTIONS} />
                        </Form.Item>
                    </Flex>
                    <Divider titlePlacement="start" plain className="!my-4">
                        下发快读
                    </Divider>
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="下发快读窗口"
                            className={pairedFormItemClassName}
                            extra="下发成功后保持快读的时长，0 表示关闭"
                        >
                            <Space.Compact block>
                                <Form.Item name="commandFastReadDuration" noStyle>
                                    <InputNumber
                                        min={0}
                                        max={3600}
                                        className={numericInputClassName}
                                    />
                                </Form.Item>
                                <Input
                                    value="秒"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：秒"
                                />
                            </Space.Compact>
                        </Form.Item>
                        <Form.Item
                            label="快读间隔"
                            className={pairedFormItemClassName}
                            extra="快读窗口内的读取间隔"
                        >
                            <Space.Compact block>
                                <Form.Item name="commandFastReadInterval" noStyle>
                                    <InputNumber
                                        min={1}
                                        max={60}
                                        className={numericInputClassName}
                                    />
                                </Form.Item>
                                <Input
                                    value="秒"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：秒"
                                />
                            </Space.Compact>
                        </Form.Item>
                    </Flex>
                    <Divider titlePlacement="start" plain className="!my-4">
                        组包参数
                    </Divider>
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="组包地址间隙"
                            className={pairedFormItemClassName}
                            extra="地址间隙 <= 该值时会合并成同一读包，0 表示只合并连续地址"
                        >
                            <Space.Compact block>
                                <Form.Item name="packetMergeGap" noStyle>
                                    <InputNumber
                                        min={0}
                                        max={2000}
                                        className={numericInputClassName}
                                    />
                                </Form.Item>
                                <Input
                                    value="寄存器"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：寄存器"
                                />
                            </Space.Compact>
                        </Form.Item>
                        <Form.Item
                            label="单包最大寄存器数"
                            className={pairedFormItemClassName}
                            extra="每个读包最多读取的字寄存器数量"
                        >
                            <Space.Compact block>
                                <Form.Item name="packetMaxQuantity" noStyle>
                                    <InputNumber
                                        min={1}
                                        max={125}
                                        className={numericInputClassName}
                                    />
                                </Form.Item>
                                <Input
                                    value="个"
                                    readOnly
                                    tabIndex={-1}
                                    className={numericUnitClassName}
                                    aria-label="单位：个"
                                />
                            </Space.Compact>
                        </Form.Item>
                    </Flex>
                    <Divider titlePlacement="start" plain className="!my-4">
                        其他
                    </Divider>
                    <Form.Item label="备注" name="remark">
                        <Input.TextArea rows={3} placeholder="备注说明" />
                    </Form.Item>
                    <Form.Item label="启用" name="enabled" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);
