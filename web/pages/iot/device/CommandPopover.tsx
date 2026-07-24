import { App, Button, Checkbox, Flex, Input } from 'antd';
import { useCallback, useState } from 'react';
import { parseDateTime } from '@/utils/dateTime';
import { useDeviceCommand } from './device.service';
import type { Device } from './device.types';

interface CommandElement {
    _key: string;
    elementId: string;
    name: string;
    value: string;
    unit?: string;
    options?: Device.CommandOperationElement['options'];
    dataType?: string;
    size?: number;
    encode?: string;
    length?: number;
    digits?: number;
}

interface CommandPopoverProps {
    device: Device.RealTimeData;
    func: Device.CommandOperation;
    onClose: () => void;
}

const INTEGER_RANGES: Record<string, [bigint, bigint]> = {
    INT8: [-128n, 127n],
    UINT8: [0n, 255n],
    INT16: [-32768n, 32767n],
    UINT16: [0n, 65535n],
    INT32: [-2147483648n, 2147483647n],
    UINT32: [0n, 4294967295n],
    INT64: [-9223372036854775808n, 9223372036854775807n],
    UINT64: [0n, 18446744073709551615n],
};

const HEX_VALUE_PATTERN = /^[0-9a-fA-F]+$/;
const INTEGER_VALUE_PATTERN = /^[+-]?\d+$/;

const parseBigIntStrict = (value: string): bigint | null => {
    if (!INTEGER_VALUE_PATTERN.test(value)) return null;
    try {
        return BigInt(value);
    } catch {
        return null;
    }
};

const validateValue = (element: CommandElement): string | null => {
    const value = element.value.trim();
    if (!value) return `「${element.name}」值不能为空`;
    if (element.dataType === 'BOOL') {
        return value === '0' || value === '1'
            ? null
            : `「${element.name}」BOOL 类型只能输入 0 或 1`;
    }
    if (element.dataType) {
        if (element.dataType === 'STRING') {
            if (typeof element.size === 'number' && element.size > 0 && value.length > element.size)
                return `「${element.name}」STRING 长度不能超过 ${element.size} 字节`;
            return null;
        }
        const range = INTEGER_RANGES[element.dataType];
        if (range) {
            const parsed = parseBigIntStrict(value);
            if (parsed === null) return `「${element.name}」请输入有效整数`;
            if (parsed < range[0] || parsed > range[1])
                return `「${element.name}」${element.dataType} 范围 ${range[0]} ~ ${range[1]}`;
            return null;
        }
        const number = Number(value);
        if (!Number.isFinite(number)) return `「${element.name}」请输入有效数字`;
        if (
            (element.dataType === 'FLOAT32' || element.dataType === 'FLOAT') &&
            !Number.isFinite(Math.fround(number))
        )
            return `「${element.name}」${element.dataType} 值超出范围`;
        return null;
    }
    if (element.encode === 'BCD') {
        const number = Number(value);
        if (!Number.isFinite(number)) return `「${element.name}」BCD 编码只能输入数字`;
        const digits = Math.max(0, Math.min(8, element.digits ?? 0));
        const length = Math.max(1, element.length ?? 1);
        if (Math.round(Math.abs(number) * 10 ** digits) >= 10 ** (length * 2))
            return `「${element.name}」BCD 编码长度超出 ${length} 字节`;
        return null;
    }
    if (element.encode) {
        if (!HEX_VALUE_PATTERN.test(value))
            return `「${element.name}」${element.encode} 编码只能输入十六进制字符`;
        if (
            typeof element.length === 'number' &&
            element.length > 0 &&
            value.length > element.length * 2
        )
            return `「${element.name}」${element.encode} 编码长度不能超过 ${element.length} 字节`;
        return null;
    }
    return Number.isFinite(Number(value)) ? null : `「${element.name}」请输入有效数字`;
};

const isDeviceOnline = (device: Device.RealTimeData) => {
    if (device.reportTime) {
        const reportTime = parseDateTime(device.reportTime);
        if (reportTime && !Number.isNaN(reportTime.getTime()))
            return Date.now() - reportTime.getTime() < (device.online_timeout || 300) * 1000;
    }
    return device.connectionState === 'online' || device.connected === true;
};

const CommandPopover = ({ device, func, onClose }: CommandPopoverProps) => {
    const { message } = App.useApp();
    const commandMutation = useDeviceCommand();
    const isSl651CompleteCommand = device.protocol_type === 'SL651';
    const [elements, setElements] = useState<CommandElement[]>(() =>
        (func.elements || []).map((element) => ({
            ...element,
            _key: String(element.elementId ?? element.name),
            value: element.value ?? '',
        }))
    );
    const [selectedKeys, setSelectedKeys] = useState<string[]>(() =>
        isSl651CompleteCommand
            ? (func.elements || []).map((element) => String(element.elementId ?? element.name))
            : []
    );

    const checkOnline = useCallback(() => {
        if (isDeviceOnline(device)) return true;
        message.warning('设备离线');
        return false;
    }, [device, message]);

    const handleSend = useCallback(() => {
        const selected = elements.filter((element) => selectedKeys.includes(element._key));
        if (!selected.length) {
            message.warning('请至少选择一个要素');
            return;
        }
        for (const element of selected) {
            const error = validateValue(element);
            if (error) {
                message.error(error);
                return;
            }
        }
        if (!checkOnline()) return;
        commandMutation.mutate(
            {
                deviceId: device.id,
                data: {
                    elements: selected.map((element) => ({
                        elementId: element.elementId,
                        value: element.value.trim(),
                    })),
                },
            },
            { onSuccess: onClose }
        );
    }, [checkOnline, commandMutation, device.id, elements, message, onClose, selectedKeys]);

    const handlePresetClick = useCallback(
        (element: CommandElement, value: string) => {
            if (isSl651CompleteCommand) {
                setElements((current) =>
                    current.map((item) => (item._key === element._key ? { ...item, value } : item))
                );
                setSelectedKeys(elements.map((item) => item._key));
                return;
            }
            if (!checkOnline()) return;
            commandMutation.mutate({
                deviceId: device.id,
                data: { elements: [{ elementId: element.elementId, value }] },
            });
        },
        [checkOnline, commandMutation, device.id, elements, isSl651CompleteCommand]
    );

    if (!elements.length) return <div className="p-3">暂无可下发要素</div>;

    return (
        <div className="max-w-[360px]">
            <div className="mb-2">
                <div>
                    设备：{device.name}（{device.device_code}）
                </div>
                <div className="text-xs text-gray-400">指令：{func.name}</div>
            </div>
            <div className="mb-2 max-h-[260px] overflow-y-auto pr-1">
                {elements.map((element) => {
                    const checked = selectedKeys.includes(element._key);
                    const hasOptions = !!element.options?.length;
                    return (
                        <div
                            key={element._key}
                            className={`mb-2 pb-2 ${hasOptions ? 'border-b border-gray-100' : ''}`}
                        >
                            <Flex align="center" className={hasOptions ? 'mb-1.5' : ''}>
                                <Checkbox
                                    checked={checked}
                                    disabled={isSl651CompleteCommand}
                                    onChange={(event) =>
                                        setSelectedKeys((current) =>
                                            event.target.checked
                                                ? [...current, element._key]
                                                : current.filter((key) => key !== element._key)
                                        )
                                    }
                                />
                                <span className="mx-1.5 flex-1">
                                    {element.name}
                                    {element.unit ? `（${element.unit}）` : ''}
                                </span>
                                <Input
                                    size="small"
                                    className="!w-[120px]"
                                    value={element.value}
                                    placeholder={hasOptions ? '或手动输入' : ''}
                                    onChange={(event) =>
                                        setElements((current) =>
                                            current.map((item) =>
                                                item._key === element._key
                                                    ? { ...item, value: event.target.value }
                                                    : item
                                            )
                                        )
                                    }
                                />
                            </Flex>
                            {hasOptions && (
                                <Flex wrap gap={6} className="ml-[26px]">
                                    <span className="mr-1 text-xs text-gray-400">预设值：</span>
                                    {element.options?.map((option) => (
                                        <Button
                                            key={option.value}
                                            size="small"
                                            type="primary"
                                            ghost
                                            loading={commandMutation.isPending}
                                            onClick={() => handlePresetClick(element, option.value)}
                                        >
                                            {option.label}
                                        </Button>
                                    ))}
                                </Flex>
                            )}
                        </div>
                    );
                })}
            </div>
            <Flex justify="flex-end" gap={8}>
                <Button size="small" onClick={onClose}>
                    取消
                </Button>
                <Button
                    size="small"
                    type="primary"
                    loading={commandMutation.isPending}
                    disabled={!selectedKeys.length}
                    onClick={handleSend}
                >
                    下发
                </Button>
            </Flex>
        </div>
    );
};

export default CommandPopover;
