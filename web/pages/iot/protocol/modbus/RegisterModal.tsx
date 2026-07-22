/**
 * Modbus 寄存器编辑弹窗（从 ModbusConfig 抽离）
 */

import { AutoComplete, Flex, Form, Input, InputNumber, Modal, Select, Switch } from 'antd';
import { forwardRef, useImperativeHandle, useMemo, useState } from 'react';
import { FormModal } from '@/components/FormModal';
import type { useProtocolConfigSave } from '../protocol.service';
import type { Modbus, ModbusDictConfig, Protocol } from '../protocol.types';
import { useFilterableGroupOptions } from '../useFilterableGroupOptions';
import {
    ByteOrderOptions,
    checkAddressConflict,
    DataTypeOptions,
    formatScaleValue,
    generateId,
    getQuantityByDataType,
    normalizeGroupName,
    normalizeModbusRegisters,
    normalizePacketConfig,
    type RegisterModalRef,
    RegisterTypeOptions,
} from './helpers';

export interface RegisterModalProps {
    types: Protocol.Item[];
    onSuccess?: () => void;
    saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

export const RegisterModal = forwardRef<RegisterModalRef, RegisterModalProps>(
    ({ types, onSuccess, saveMutation }, ref) => {
        const [open, setOpen] = useState(false);
        const [mode, setMode] = useState<'create' | 'edit'>('create');
        const [typeId, setTypeId] = useState<string>();
        const [current, setCurrent] = useState<Modbus.Register>();
        const [form] = Form.useForm();

        // 监听寄存器类型和数据类型变化
        const registerType = Form.useWatch('registerType', form);
        const dataType = Form.useWatch('dataType', form);
        const groupNames = useMemo(() => {
            const currentType = types.find((t) => t.id === typeId);
            const config = currentType?.config as Modbus.Config | undefined;
            const groups = new Set<string>();

            for (const register of config?.registers || []) {
                const group = normalizeGroupName(register.group);
                if (group) groups.add(group);
            }

            const currentGroup = normalizeGroupName(current?.group);
            if (currentGroup) groups.add(currentGroup);

            return Array.from(groups);
        }, [current?.group, typeId, types]);
        const groupOptions = useFilterableGroupOptions(groupNames);

        useImperativeHandle(ref, () => ({
            open(m, t, register) {
                setMode(m);
                setTypeId(t);
                setCurrent(register);
                form.resetFields();
                if (register) {
                    form.setFieldsValue({
                        ...register,
                        group: normalizeGroupName(register.group) || undefined,
                        scale: typeof register.scale === 'number' ? register.scale : 1,
                        boolLabel0: register.dictConfig?.items?.find((i) => i.key === '0')?.label,
                        boolLabel1: register.dictConfig?.items?.find((i) => i.key === '1')?.label,
                    });
                } else {
                    form.setFieldsValue({
                        registerType: 'HOLDING_REGISTER',
                        dataType: 'INT16',
                        writable: false,
                        scale: 1,
                    });
                }
                setOpen(true);
            },
        }));

        const handleOk = async () => {
            if (!typeId) return;
            const values = await form.validateFields();

            const type = types.find((t) => t.id === typeId);
            if (!type) return;

            const config = type.config as Modbus.Config;
            const registers = normalizeModbusRegisters(config.registers);
            const actualQuantity = getQuantityByDataType(values.dataType);

            // 检查地址 + 数量是否溢出 uint16 范围
            if (values.address + actualQuantity - 1 > 65535) {
                Modal.error({
                    title: '地址溢出',
                    content: `地址 ${values.address} + 数量 ${actualQuantity} 超出范围（末地址不能超过 65535）`,
                });
                return;
            }

            // 检查地址冲突
            const conflictCheck = checkAddressConflict(
                registers,
                {
                    registerType: values.registerType,
                    address: values.address,
                    quantity: actualQuantity,
                },
                mode === 'edit' ? current?.id : undefined
            );

            if (conflictCheck.conflict) {
                const reg = conflictCheck.conflictWith;
                if (!reg) return;
                Modal.error({
                    title: '地址冲突',
                    content: `与寄存器「${reg.name}」地址范围冲突 (地址 ${reg.address}-${reg.address + reg.quantity - 1})`,
                });
                return;
            }

            // 构建 dictConfig（Bool 值映射；非 BOOL 类型保留已有配置）
            let dictConfig: ModbusDictConfig | undefined;
            if (values.dataType === 'BOOL') {
                dictConfig =
                    values.boolLabel0 || values.boolLabel1
                        ? {
                              items: [
                                  ...(values.boolLabel0
                                      ? [{ key: '0', label: values.boolLabel0 }]
                                      : []),
                                  ...(values.boolLabel1
                                      ? [{ key: '1', label: values.boolLabel1 }]
                                      : []),
                              ],
                          }
                        : undefined;
            } else {
                // 非 BOOL 类型：编辑时保留已有 dictConfig，创建时无
                dictConfig = mode === 'edit' ? current?.dictConfig : undefined;
            }

            // 仅 COIL/HOLDING_REGISTER 可配置 writable，其他类型始终 false
            const isWritableType =
                values.registerType === 'COIL' || values.registerType === 'HOLDING_REGISTER';
            const writable = isWritableType ? !!values.writable : false;
            const inputScale = Number(values.scale);
            const scale =
                values.registerType === 'COIL' || values.registerType === 'DISCRETE_INPUT'
                    ? 1
                    : Number.isFinite(inputScale) && inputScale > 0
                      ? inputScale
                      : 1;
            const group = normalizeGroupName(values.group);

            const registerFields = {
                name: values.name,
                group: group || undefined,
                registerType: values.registerType,
                address: values.address,
                dataType: values.dataType,
                byteOrder:
                    values.registerType === 'HOLDING_REGISTER' ||
                    values.registerType === 'INPUT_REGISTER'
                        ? values.byteOrder || undefined
                        : undefined,
                quantity: actualQuantity,
                writable,
                unit: values.unit,
                scale,
                decimals: values.decimals,
                dictConfig,
                remark: values.remark,
            };

            let newRegisters: Modbus.Register[];

            if (mode === 'create') {
                newRegisters = [...registers, { id: generateId(), ...registerFields }];
            } else {
                newRegisters = registers.map((r) =>
                    r.id === current?.id ? { ...r, ...registerFields } : r
                );
            }

            await saveMutation.mutateAsync({
                id: typeId,
                protocol: 'Modbus',
                config: {
                    byteOrder: config.byteOrder,
                    readInterval: config.readInterval,
                    packet: normalizePacketConfig(config.packet),
                    registers: newRegisters,
                },
            });

            onSuccess?.();
            setOpen(false);
        };

        // 线圈/离散输入只支持 BOOL；保持寄存器/输入寄存器不支持 BOOL
        const isBitRegister = registerType === 'COIL' || registerType === 'DISCRETE_INPUT';
        const isWordRegister =
            registerType === 'HOLDING_REGISTER' || registerType === 'INPUT_REGISTER';

        return (
            <FormModal
                title={mode === 'create' ? '新增寄存器' : '编辑寄存器'}
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
                        <Input placeholder="如：温度、湿度、电压" />
                    </Form.Item>

                    <Form.Item
                        label="分组"
                        name="group"
                        extra="同一分组的寄存器会在配置页聚合为同一组卡片，留空则显示在未分组中"
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

                    <Flex gap={16}>
                        <Form.Item
                            label="寄存器类型"
                            name="registerType"
                            rules={[{ required: true, message: '请选择寄存器类型' }]}
                            className="flex-1"
                        >
                            <Select
                                options={RegisterTypeOptions}
                                onChange={(val) => {
                                    if (val === 'COIL' || val === 'DISCRETE_INPUT') {
                                        form.setFieldsValue({
                                            dataType: 'BOOL',
                                            decimals: undefined,
                                            writable: false,
                                            scale: 1,
                                            byteOrder: undefined,
                                        });
                                    } else if (form.getFieldValue('dataType') === 'BOOL') {
                                        form.setFieldsValue({
                                            dataType: 'INT16',
                                            decimals: undefined,
                                            boolLabel0: undefined,
                                            boolLabel1: undefined,
                                        });
                                    }
                                    // 切换到只读类型时清除 writable
                                    if (val === 'DISCRETE_INPUT' || val === 'INPUT_REGISTER') {
                                        form.setFieldsValue({ writable: false });
                                    }
                                }}
                            />
                        </Form.Item>
                        <Form.Item
                            label="地址"
                            name="address"
                            rules={[{ required: true, message: '请输入地址' }]}
                            className="flex-1"
                        >
                            <InputNumber
                                min={0}
                                max={65535}
                                className="!w-full"
                                placeholder="0-65535"
                            />
                        </Form.Item>
                    </Flex>

                    <Form.Item
                        label="数据类型"
                        name="dataType"
                        rules={[{ required: true, message: '请选择数据类型' }]}
                    >
                        <Select
                            options={
                                isBitRegister
                                    ? [
                                          {
                                              value: 'BOOL' as const,
                                              label: 'BOOL (1 bit)',
                                              quantity: 1,
                                          },
                                      ]
                                    : isWordRegister
                                      ? DataTypeOptions.filter((o) => o.value !== 'BOOL')
                                      : DataTypeOptions
                            }
                            disabled={isBitRegister}
                            onChange={() => {
                                form.setFieldsValue({
                                    decimals: undefined,
                                    boolLabel0: undefined,
                                    boolLabel1: undefined,
                                });
                            }}
                        />
                    </Form.Item>

                    {isWordRegister && (
                        <Form.Item
                            label="字节序"
                            name="byteOrder"
                            extra="留空时继承设备配置；选择后仅覆盖当前寄存器的读写字节序"
                        >
                            <Select
                                allowClear
                                options={ByteOrderOptions}
                                placeholder="继承设备配置"
                            />
                        </Form.Item>
                    )}

                    {dataType === 'BOOL' && (
                        <Flex gap={16}>
                            <Form.Item label="0 值显示" name="boolLabel0" className="flex-1">
                                <Input placeholder="如：关闭、OFF" />
                            </Form.Item>
                            <Form.Item label="1 值显示" name="boolLabel1" className="flex-1">
                                <Input placeholder="如：开启、ON" />
                            </Form.Item>
                        </Flex>
                    )}

                    <Flex gap={16}>
                        <Form.Item label="单位" name="unit" className="flex-1">
                            <Input placeholder="如：V、A、℃、%" />
                        </Form.Item>
                        <Form.Item
                            label="缩放系数"
                            name="scale"
                            className="flex-1"
                            extra="入库值 = 原始值 × 缩放系数（默认 1）"
                            rules={[
                                {
                                    validator: async (_, value) => {
                                        const numericValue = Number(value);
                                        if (!Number.isFinite(numericValue) || numericValue <= 0) {
                                            throw new Error('请输入大于 0 的缩放系数');
                                        }
                                    },
                                },
                            ]}
                        >
                            <InputNumber
                                min={0.000001 as number}
                                max={1000000 as number}
                                step={0.1}
                                precision={6}
                                formatter={(value) => formatScaleValue(value)}
                                parser={(value) => Number(value || 1)}
                                className="!w-full"
                                disabled={isBitRegister}
                            />
                        </Form.Item>
                        {(dataType === 'FLOAT32' || dataType === 'DOUBLE') && (
                            <Form.Item label="小数位数" name="decimals" className="flex-1">
                                <InputNumber
                                    min={0}
                                    max={8}
                                    placeholder="不限制"
                                    className="!w-full"
                                />
                            </Form.Item>
                        )}
                    </Flex>

                    {(registerType === 'COIL' || registerType === 'HOLDING_REGISTER') && (
                        <Form.Item
                            label="可写"
                            name="writable"
                            valuePropName="checked"
                            extra="开启后该寄存器可用于写操作下发"
                        >
                            <Switch />
                        </Form.Item>
                    )}

                    <Form.Item label="备注" name="remark">
                        <Input placeholder="备注信息" />
                    </Form.Item>
                </Form>
            </FormModal>
        );
    }
);
