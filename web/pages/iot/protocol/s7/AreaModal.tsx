/**
 * S7 寄存器（Area）编辑弹窗（从 S7Config 抽离）
 */

import { AutoComplete, Col, Flex, Form, Input, InputNumber, Row, Select, Switch } from 'antd';
import { useEffect, useMemo } from 'react';
import { FormModal } from '@/components/FormModal';
import { normalizeGroupName } from '../grouping';
import type { S7 } from '../protocol.types';
import { useFilterableGroupOptions } from '../useFilterableGroupOptions';
import {
    areaAddressHintMap,
    bitOnlyAreaTypes,
    generateId,
    getAddressRuleText,
    getAddressSuffixExample,
    getAreaAddressSample,
    getAreaDataTypeOptions,
    getAreaTypeOptions,
    getDataTypeSize,
    normalizeAreaTypeForPlcModel,
    normalizeS7DataType,
    supportsBitAddress,
    supportsS7Decimals,
    writableAreaTypes,
} from './helpers';

export interface AreaModalProps {
    open: boolean;
    mode: 'create' | 'edit';
    initialValue?: S7.Area;
    plcModel?: S7.PlcModel;
    groupOptions?: string[];
    onCancel: () => void;
    onSubmit: (value: S7.Area) => void;
}

export function AreaModal({
    open,
    mode,
    initialValue,
    plcModel,
    groupOptions = [],
    onCancel,
    onSubmit,
}: AreaModalProps) {
    const [form] = Form.useForm<S7.Area>();
    const areaType = Form.useWatch('area', form);
    const dataType = normalizeS7DataType(Form.useWatch('dataType', form) as string | undefined);
    const dbNumber = Form.useWatch('dbNumber', form);
    const startAddress = Form.useWatch('start', form);
    const startBit = Form.useWatch('startBit', form);
    const stringLength = Form.useWatch('size', form);
    const parsedAreaType = areaType as S7.AreaType | undefined;
    const areaTypeOptions = getAreaTypeOptions(plcModel);
    const defaultAreaType = areaTypeOptions[0]?.value ?? 'DB';
    const normalizedInitialArea =
        normalizeAreaTypeForPlcModel(plcModel, initialValue?.area) ?? defaultAreaType;
    const initialDataType = normalizeS7DataType(initialValue?.dataType as string | undefined);
    const initialDbNumber =
        normalizedInitialArea === 'V'
            ? undefined
            : (initialValue?.dbNumber ?? (normalizedInitialArea === 'DB' ? 1 : undefined));
    const initialFormValues = useMemo<Partial<S7.Area>>(
        () =>
            initialValue
                ? {
                      ...initialValue,
                      area: normalizedInitialArea,
                      dbNumber: initialDbNumber,
                      dataType: initialDataType,
                      decimals: supportsS7Decimals(initialDataType)
                          ? initialValue.decimals
                          : undefined,
                      group: initialValue.group,
                  }
                : {
                      name: '',
                      group: undefined,
                      area: defaultAreaType,
                      dbNumber: initialDbNumber,
                      dataType: 'INT16' as S7.AreaDataType,
                      start: 0,
                      size: 1,
                      unit: undefined,
                      decimals: undefined,
                      writable: false,
                      remark: '',
                      startBit: undefined,
                  },
        [defaultAreaType, initialDataType, initialDbNumber, initialValue, normalizedInitialArea]
    );
    const groupOptionList = useFilterableGroupOptions(groupOptions);
    const isStringType = dataType === 'STRING';
    const isBoolDataType = dataType === 'BOOL';
    const canUseDecimals = supportsS7Decimals(dataType);
    const isWritableArea = writableAreaTypes.includes(parsedAreaType as S7.AreaType);
    const showBitInput = supportsBitAddress(parsedAreaType, dataType);
    const addressExample = getAddressSuffixExample(parsedAreaType, dataType);
    const calcPreviewSize =
        dataType === 'STRING'
            ? typeof stringLength === 'number' && stringLength > 0
                ? stringLength
                : 1
            : getDataTypeSize(dataType);
    const startOffset = typeof startAddress === 'number' && startAddress >= 0 ? startAddress : 0;
    const endOffset =
        dataType === 'STRING'
            ? startOffset + Math.max(calcPreviewSize, 1) - 1
            : dataType === 'LREAL'
              ? startOffset + 1
              : startOffset;
    const addressSample = getAreaAddressSample(
        parsedAreaType,
        dataType,
        dbNumber,
        startOffset,
        startBit
    );
    useEffect(() => {
        if (!open) {
            form.resetFields();
            return;
        }
        form.resetFields();
        form.setFieldsValue(initialFormValues);
    }, [form, initialFormValues, open]);
    const endAddressSample = getAreaAddressSample(
        parsedAreaType,
        dataType,
        dbNumber,
        endOffset,
        startBit
    );
    const canUseBoolAddress =
        parsedAreaType === 'DB' ||
        parsedAreaType === 'V' ||
        parsedAreaType === 'MK' ||
        parsedAreaType === 'PE' ||
        parsedAreaType === 'PA';

    const handleOk = async () => {
        const values = await form.validateFields();
        const resolvedDataType =
            parsedAreaType === 'CT' || parsedAreaType === 'TM'
                ? values.dataType || 'UINT16'
                : parsedAreaType === 'PE' || parsedAreaType === 'PA'
                  ? 'BOOL'
                  : values.dataType;
        const size =
            resolvedDataType === 'STRING' ? values.size : getDataTypeSize(resolvedDataType);
        const nextDbNumber = parsedAreaType === 'DB' ? values.dbNumber : undefined;
        const nextStartBit = isBoolDataType && canUseBoolAddress ? values.startBit : undefined;
        const nextDecimals =
            supportsS7Decimals(resolvedDataType) && typeof values.decimals === 'number'
                ? values.decimals
                : undefined;
        const nextValues = {
            ...values,
            group: normalizeGroupName(values.group) || undefined,
            dbNumber: nextDbNumber,
            startBit: nextStartBit,
            decimals: nextDecimals,
            dataType: resolvedDataType,
            id: mode === 'create' ? generateId() : initialValue?.id || values.id,
            size,
            writable: isWritableArea ? values.writable : false,
        };
        onSubmit(nextValues);
    };

    return (
        <FormModal
            title={mode === 'create' ? '新增寄存器' : '编辑寄存器'}
            open={open}
            onCancel={onCancel}
            onOk={handleOk}
            destroyOnHidden
        >
            <Form form={form} layout="vertical" initialValues={initialFormValues}>
                <Form.Item
                    name="name"
                    label="寄存器名称"
                    rules={[{ required: true, message: '请输入寄存器名称' }]}
                >
                    <Input placeholder="例如: 温度寄存器" />
                </Form.Item>
                <Form.Item
                    name="group"
                    label="分组"
                    extra="同一分组的寄存器会在配置页聚合为同一组卡片，留空则显示在未分组中"
                >
                    <AutoComplete
                        allowClear
                        options={groupOptionList.options}
                        placeholder="例如: 基础信息、告警、控制"
                        filterOption={false}
                        onDropdownVisibleChange={groupOptionList.onDropdownVisibleChange}
                        onSearch={groupOptionList.onSearch}
                    />
                </Form.Item>
                <Row gutter={12}>
                    <Col xs={24} sm={12}>
                        <Form.Item
                            name="area"
                            label="寄存器类型"
                            rules={[{ required: true }]}
                            extra={
                                plcModel === 'S7-200'
                                    ? 'S7-200 使用 V/M/I/Q/C/T 区域'
                                    : '不同区域类型映射不同读取方式'
                            }
                        >
                            <Select
                                options={areaTypeOptions}
                                onChange={(value: S7.AreaType) => {
                                    const currentDataType = normalizeS7DataType(
                                        form.getFieldValue('dataType') as string | undefined
                                    );
                                    const updates: Partial<S7.Area> = {};
                                    const nextDataType =
                                        value === 'CT' || value === 'TM'
                                            ? 'UINT16'
                                            : bitOnlyAreaTypes.includes(value)
                                              ? 'BOOL'
                                              : currentDataType;

                                    if (value === 'CT' || value === 'TM') {
                                        updates.dataType = nextDataType;
                                        updates.size = getDataTypeSize(nextDataType);
                                    } else if (bitOnlyAreaTypes.includes(value)) {
                                        updates.dataType = nextDataType;
                                        updates.size = getDataTypeSize('BOOL');
                                    } else if (nextDataType !== 'STRING') {
                                        updates.size = getDataTypeSize(nextDataType);
                                    }

                                    if (value !== 'DB') {
                                        updates.dbNumber = undefined;
                                    }
                                    if (value === 'DB' && form.getFieldValue('dbNumber') == null) {
                                        updates.dbNumber = 1;
                                    }
                                    updates.startBit = undefined;
                                    if (!supportsS7Decimals(nextDataType)) {
                                        updates.decimals = undefined;
                                    }

                                    if (Object.keys(updates).length > 0) {
                                        form.setFieldsValue(updates);
                                    }
                                }}
                            />
                        </Form.Item>
                    </Col>
                    <Col xs={24} sm={12}>
                        <Form.Item
                            name="dataType"
                            label="数据类型"
                            rules={[{ required: true }]}
                            extra="不同数据类型对应不同解析方式"
                        >
                            <Select
                                options={getAreaDataTypeOptions(
                                    areaType as S7.AreaType | undefined
                                )}
                                onChange={(value: S7.AreaDataType) => {
                                    const updates: Partial<S7.Area> = {};
                                    if (value !== 'BOOL') {
                                        updates.startBit = undefined;
                                    }
                                    if (value === 'STRING') {
                                        const currentSize = form.getFieldValue('size') as
                                            | number
                                            | undefined;
                                        if (
                                            typeof currentSize !== 'number' ||
                                            Number.isNaN(currentSize)
                                        ) {
                                            updates.size = 1;
                                        }
                                    } else {
                                        updates.size = getDataTypeSize(value);
                                    }
                                    if (!supportsS7Decimals(value)) {
                                        updates.decimals = undefined;
                                    }
                                    if (Object.keys(updates).length > 0) {
                                        form.setFieldsValue(updates);
                                    }
                                }}
                            />
                        </Form.Item>
                    </Col>
                </Row>
                {areaType === 'DB' && (
                    <Row gutter={12}>
                        <Col xs={24} sm={12}>
                            <Form.Item
                                name="dbNumber"
                                label="DB 编号"
                                rules={[{ required: true, message: '请输入 DB 编号' }]}
                                extra="示例：S7-300 上可读 DB1~DB999；S7-1200 常见为 DB1、DB100 等"
                            >
                                <InputNumber min={1} className="w-full" />
                            </Form.Item>
                        </Col>
                    </Row>
                )}
                <Flex gap={16} className="w-full" align="flex-start" wrap>
                    <Form.Item
                        name="start"
                        label="起始偏移"
                        rules={[
                            {
                                required: true,
                                message: getAddressRuleText(parsedAreaType, isBoolDataType),
                            },
                            { type: 'number', min: 0, message: '偏移不能小于 0' },
                        ]}
                        className="flex-1"
                    >
                        <InputNumber min={0} className="!w-full" />
                    </Form.Item>
                    {showBitInput && (
                        <Form.Item
                            name="startBit"
                            label="位号"
                            rules={
                                isBoolDataType
                                    ? [
                                          { required: true, message: '请输入位号' },
                                          {
                                              type: 'number',
                                              min: 0,
                                              max: 7,
                                              message: '位号只能是 0~7',
                                          },
                                      ]
                                    : undefined
                            }
                            className="flex-1"
                        >
                            <InputNumber min={0} max={7} className="!w-full" />
                        </Form.Item>
                    )}
                    {isStringType && (
                        <Form.Item
                            name="size"
                            label="长度（字节）"
                            rules={[{ required: true, message: '请输入字符串长度' }]}
                            className="flex-1"
                            extra="字符串长度单位为字节"
                        >
                            <InputNumber min={1} className="!w-full" />
                        </Form.Item>
                    )}
                </Flex>
                <Form.Item label="地址示例">
                    <div className="text-xs text-gray-500">
                        参考示例：
                        {addressExample ||
                            (parsedAreaType ? '按区域规则自动拼接' : '请先选择寄存器类型')}
                    </div>
                    <div className="text-xs text-gray-500">
                        {areaType ? `示例：${areaAddressHintMap[areaType]}` : '请先选择寄存器类型'}
                    </div>
                    <div className="text-xs text-gray-500">
                        当前起始地址：{addressSample || '暂无'}
                    </div>
                    <div className="text-xs text-gray-500">
                        当前结束地址：{endAddressSample || '暂无'}
                    </div>
                </Form.Item>
                <Row gutter={12}>
                    <Col xs={24} sm={canUseDecimals ? 12 : 24}>
                        <Form.Item name="unit" label="单位">
                            <Input placeholder="如：V、A、℃、%" />
                        </Form.Item>
                    </Col>
                    {canUseDecimals && (
                        <Col xs={24} sm={12}>
                            <Form.Item name="decimals" label="小数位数">
                                <InputNumber
                                    min={0}
                                    max={8}
                                    placeholder="例如 2"
                                    className="!w-full"
                                />
                            </Form.Item>
                        </Col>
                    )}
                </Row>
                {isWritableArea && (
                    <Form.Item name="writable" label="可写" valuePropName="checked">
                        <Switch />
                    </Form.Item>
                )}
                <Form.Item name="remark" label="备注">
                    <Input.TextArea rows={3} placeholder="备注说明" />
                </Form.Item>
            </Form>
        </FormModal>
    );
}
