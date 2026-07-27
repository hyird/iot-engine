import { CloseOutlined } from '@ant-design/icons';
import { Button, Input, InputNumber, Select } from 'antd';
import type { Alert } from './alert.types';

const CONDITION_TYPE_OPTIONS = [
    { label: '阈值', value: 'threshold' },
    { label: '离线检测', value: 'offline' },
    { label: '变化率', value: 'rate_of_change' },
];

const OPERATOR_OPTIONS = [
    { label: '>', value: '>' },
    { label: '>=', value: '>=' },
    { label: '<', value: '<' },
    { label: '<=', value: '<=' },
    { label: '==', value: '==' },
    { label: '!=', value: '!=' },
];

const DIRECTION_OPTIONS = [
    { label: '任意', value: 'any' },
    { label: '上升', value: 'rise' },
    { label: '下降', value: 'fall' },
];

/** 字典映射项（简化版，同时适用 SL651 和 Modbus） */
export interface DictItem {
    key: string;
    label: string;
    /** 触发值（仅 BIT 模式，"0"或"1"） */
    value?: string;
}

export interface ElementOption {
    value: string;
    label: string;
    /** 字典映射类型：BIT=位映射 VALUE=值映射（仅 SL651 DICT 要素） */
    dictMapType?: 'VALUE' | 'BIT';
    /** 字典映射项 */
    dictItems?: DictItem[];
}

interface ConditionEditorProps {
    value: Alert.Condition;
    elementOptions: ElementOption[];
    onChange: (value: Alert.Condition) => void;
    onRemove: () => void;
}

export function ConditionEditor({
    value,
    elementOptions,
    onChange,
    onRemove,
}: ConditionEditorProps) {
    const update = (patch: Partial<Alert.Condition>) => {
        onChange({ ...value, ...patch });
    };

    const hasElements = elementOptions.length > 0;

    // 当前选中要素的字典信息
    const selectedElement = elementOptions.find((e) => e.value === value.elementKey);
    const isBitDict = selectedElement?.dictMapType === 'BIT' && !!selectedElement.dictItems?.length;
    const isValueDict =
        !!selectedElement?.dictItems?.length && selectedElement.dictMapType !== 'BIT';

    // 要素选择器（threshold 和 rate_of_change 共用）
    const renderElementSelect = () => (
        <div className="flex-1 min-w-0">
            {hasElements ? (
                <Select
                    value={value.elementKey || undefined}
                    onChange={(v: string) => update({ elementKey: v, bitIndex: undefined })}
                    options={elementOptions}
                    showSearch
                    optionFilterProp="label"
                    placeholder="选择要素"
                    className="w-full"
                />
            ) : (
                <Input
                    value={value.elementKey}
                    onChange={(e) => update({ elementKey: e.target.value })}
                    placeholder="要素标识"
                />
            )}
        </div>
    );

    return (
        <div className="border border-gray-200 rounded-md p-3 mb-2 relative">
            <Button
                type="text"
                size="small"
                danger
                icon={<CloseOutlined />}
                onClick={onRemove}
                className="absolute top-1 right-1"
            />

            <div className="flex items-start gap-2">
                <div className="w-[100px] shrink-0">
                    <Select
                        value={value.type}
                        onChange={(type: Alert.ConditionType) => {
                            if (type === 'threshold') {
                                onChange({ type, elementKey: '', operator: '>', value: '' });
                            } else if (type === 'offline') {
                                onChange({ type, duration: 300 });
                            } else {
                                onChange({
                                    type,
                                    elementKey: '',
                                    changeRate: '',
                                    changeDirection: 'any',
                                });
                            }
                        }}
                        options={CONDITION_TYPE_OPTIONS}
                        placeholder="条件类型"
                        className="w-full"
                    />
                </div>

                {value.type === 'threshold' && (
                    <>
                        {renderElementSelect()}
                        {isBitDict && (
                            <div className="w-[120px] shrink-0">
                                <Select
                                    value={
                                        value.bitIndex != null ? String(value.bitIndex) : undefined
                                    }
                                    onChange={(v: string) => update({ bitIndex: Number(v) })}
                                    options={selectedElement?.dictItems?.map((d) => ({
                                        value: d.key,
                                        label: `${d.label}(${d.key})`,
                                    }))}
                                    placeholder="选择位"
                                    className="w-full"
                                />
                            </div>
                        )}
                        <div className="w-[70px] shrink-0">
                            <Select
                                value={value.operator}
                                onChange={(operator: Alert.Operator) => update({ operator })}
                                options={OPERATOR_OPTIONS}
                                className="w-full"
                            />
                        </div>
                        <div className="w-[130px] shrink-0">
                            {isValueDict ? (
                                <Select
                                    value={value.value || undefined}
                                    onChange={(v: string) => update({ value: v })}
                                    options={selectedElement?.dictItems?.map((d) => ({
                                        value: d.key,
                                        label: d.label,
                                    }))}
                                    placeholder="选择值"
                                    className="w-full"
                                />
                            ) : (
                                <Input
                                    value={value.value}
                                    onChange={(e) => update({ value: e.target.value })}
                                    placeholder="阈值"
                                />
                            )}
                        </div>
                    </>
                )}

                {value.type === 'offline' && (
                    <div className="flex-1 min-w-0">
                        <InputNumber
                            value={value.duration}
                            onChange={(v) => update({ duration: v ?? 300 })}
                            addonAfter="秒"
                            min={60}
                            max={86400}
                            placeholder="离线超时时间"
                            className="w-full"
                        />
                    </div>
                )}

                {value.type === 'rate_of_change' && (
                    <>
                        {renderElementSelect()}
                        {isBitDict && (
                            <div className="w-[120px] shrink-0">
                                <Select
                                    value={
                                        value.bitIndex != null ? String(value.bitIndex) : undefined
                                    }
                                    onChange={(v: string) => update({ bitIndex: Number(v) })}
                                    options={selectedElement?.dictItems?.map((d) => ({
                                        value: d.key,
                                        label: `${d.label}(${d.key})`,
                                    }))}
                                    placeholder="选择位"
                                    className="w-full"
                                />
                            </div>
                        )}
                        <div className="w-[100px] shrink-0">
                            <Input
                                value={value.changeRate}
                                onChange={(e) => update({ changeRate: e.target.value })}
                                placeholder="变化率%"
                            />
                        </div>
                        <div className="w-[100px] shrink-0">
                            <Select
                                value={value.changeDirection}
                                onChange={(changeDirection: Alert.ChangeDirection) =>
                                    update({ changeDirection })
                                }
                                options={DIRECTION_OPTIONS}
                                className="w-full"
                            />
                        </div>
                    </>
                )}
            </div>
        </div>
    );
}
