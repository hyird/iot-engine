/**
 * Modbus 设备类型编辑弹窗（从 ModbusConfig 抽离）
 */

import { Flex, Form, Input, InputNumber, Select, Space, Switch } from 'antd';
import { forwardRef, useImperativeHandle, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { useProtocolConfigSave } from '../protocol.service';
import type { Modbus, Protocol } from '../protocol.types';
import {
    ByteOrderOptions,
    DEFAULT_PACKET_MAX_QUANTITY,
    DEFAULT_PACKET_MERGE_GAP,
    type DeviceTypeModalRef,
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
                form.resetFields();
                if (data) {
                    const config = data.config as Modbus.Config;
                    const packet = normalizePacketConfig(config?.packet);
                    form.setFieldsValue({
                        name: data.name,
                        enabled: data.enabled,
                        byteOrder: config?.byteOrder || 'BIG_ENDIAN',
                        readInterval: Number(config?.readInterval) || 1,
                        storageInterval: Number(config?.storageInterval) || 1,
                        commandFastReadDuration: Number(config?.commandFastReadDuration ?? 60),
                        commandFastReadInterval: Number(config?.commandFastReadInterval ?? 1),
                        packetMergeGap: packet.mergeGap,
                        packetMaxQuantity: packet.maxQuantity,
                        remark: data.remark,
                    });
                } else {
                    form.setFieldsValue({
                        enabled: true,
                        byteOrder: 'BIG_ENDIAN',
                        readInterval: 1,
                        storageInterval: 1,
                        commandFastReadDuration: 60,
                        commandFastReadInterval: 1,
                        packetMergeGap: DEFAULT_PACKET_MERGE_GAP,
                        packetMaxQuantity: DEFAULT_PACKET_MAX_QUANTITY,
                    });
                }
                setOpen(true);
            },
        }));

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
                    storageInterval: values.storageInterval,
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
                <Form form={form} layout="vertical">
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
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="读取间隔（秒）"
                            name="readInterval"
                            extra="数值越小采集越频繁，建议按设备负载设置间隔"
                            className={pairedFormItemClassName}
                        >
                            <Space.Compact block>
                                <InputNumber min={1} max={3600} className={numericInputClassName} />
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
                            label="存储间隔（秒）"
                            name="storageInterval"
                            extra="历史数据入库的最小间隔，1 表示每次读取都存储"
                            className={pairedFormItemClassName}
                        >
                            <Space.Compact block>
                                <InputNumber
                                    min={1}
                                    max={86400}
                                    className={numericInputClassName}
                                />
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
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="下发快读窗口"
                            name="commandFastReadDuration"
                            className={pairedFormItemClassName}
                            extra="下发成功后保持快读的时长，0 表示关闭"
                        >
                            <Space.Compact block>
                                <InputNumber min={0} max={3600} className={numericInputClassName} />
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
                            name="commandFastReadInterval"
                            className={pairedFormItemClassName}
                            extra="快读窗口内的读取间隔"
                        >
                            <Space.Compact block>
                                <InputNumber min={1} max={60} className={numericInputClassName} />
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
                    <Flex gap={16} align="start">
                        <Form.Item
                            label="组包地址间隙"
                            name="packetMergeGap"
                            className={pairedFormItemClassName}
                            extra="地址间隙 <= 该值时会合并成同一读包，0 表示只合并连续地址"
                        >
                            <Space.Compact block>
                                <InputNumber min={0} max={2000} className={numericInputClassName} />
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
                            name="packetMaxQuantity"
                            className={pairedFormItemClassName}
                            extra="每个读包最多读取的字寄存器数量"
                        >
                            <Space.Compact block>
                                <InputNumber min={1} max={125} className={numericInputClassName} />
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
