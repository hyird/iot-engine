/**
 * 设备表单弹窗组件
 */

import { Collapse, Form, Input, InputNumber, Radio, Select, Switch, TreeSelect } from 'antd';
import type { RuleObject } from 'antd/es/form';
import { useEffect, useMemo, useState } from 'react';
import { FormModal } from '@/components/FormModal';

import type { Link } from '../link/link.types';
import { useProtocolConfigOptions } from '../protocol/protocol.service';
import type { Protocol } from '../protocol/protocol.types';
import { useAgentEndpoints, useAgentOptions, useDeviceGroupTree } from './device.service';
import type { Device } from './device.types';
import type { DeviceGroup } from './device-group.types';

/** HEX 内容校验规则 */
const hexContentRules: RuleObject[] = [
    { required: true, message: '请输入十六进制内容' },
    { pattern: /^[0-9A-Fa-f\s]+$/, message: '只能包含十六进制字符 (0-9, A-F)' },
    {
        validator: (_, value: string) => {
            if (!value) return Promise.resolve();
            const stripped = value.replace(/\s/g, '');
            if (stripped.length === 0) return Promise.reject('内容不能为空');
            if (stripped.length % 2 !== 0)
                return Promise.reject('十六进制长度必须为偶数（完整字节）');
            return Promise.resolve();
        },
    },
];

const TIMEZONE_OPTIONS = Array.from({ length: 105 }, (_, index) => {
    const minutes = -12 * 60 + index * 15;
    const sign = minutes >= 0 ? '+' : '-';
    const absolute = Math.abs(minutes);
    const value = `${sign}${String(Math.floor(absolute / 60)).padStart(2, '0')}:${String(absolute % 60).padStart(2, '0')}`;
    return {
        value,
        label: `UTC${sign}${Math.floor(absolute / 60)}:${String(absolute % 60).padStart(2, '0')}`,
    };
});

/** 连接方式 */
type ConnectionMode = 'link' | 'agent';

interface DeviceFormValues {
    id?: string;
    name: string;
    device_code: string;
    connection_mode: ConnectionMode;
    link_id: string;
    target_id?: string;
    // Agent 模式字段
    agent_id?: string;
    agent_endpoint_id?: string;

    protocol_config_id: string;
    status: Device.Status;
    online_timeout?: number;
    remote_control?: boolean;
    modbus_mode?: Device.ModbusMode;
    slave_id?: number;
    timezone?: string;
    heartbeat?: Device.HeartbeatConfig;
    registration?: Device.RegistrationConfig;
    group_id?: string | null;
    remark?: string;
}

interface DeviceFormModalProps {
    open: boolean;
    editing: Device.Item | null;
    loading: boolean;
    linkOptions: Link.Option[];
    onCancel: () => void;
    onFinish: (values: DeviceFormValues) => void;
}

/** 链路协议 → 协议配置类型 映射 */
const toProtocolType = (linkProtocol?: string): Protocol.Type | undefined => {
    if (!linkProtocol) return undefined;
    if (linkProtocol === 'SL651') return 'SL651';
    if (linkProtocol === 'S7') return 'S7';
    return 'Modbus';
};

const DeviceFormModal = ({
    open,
    editing,
    loading,
    linkOptions,
    onCancel,
    onFinish,
}: DeviceFormModalProps) => {
    const [form] = Form.useForm<DeviceFormValues>();
    const { data: groupTree = [] } = useDeviceGroupTree();
    const { data: agentListData } = useAgentOptions({ enabled: open });

    const agentOptions = useMemo(() => agentListData ?? [], [agentListData]);

    const connectionMode = Form.useWatch('connection_mode', form);
    const linkId = Form.useWatch('link_id', form);
    const agentId = Form.useWatch('agent_id', form);

    // 本地链路模式：协议类型从选中链路推导
    const selectedLink = linkOptions.find((opt) => opt.id === linkId);
    const linkProtocolType = toProtocolType(selectedLink?.protocol);

    // Agent 端点列表
    const { data: agentEndpoints = [] } = useAgentEndpoints(agentId, {
        enabled: !!agentId && connectionMode === 'agent',
    });

    // Agent 模式：用 React state 管理端点协议和模式（Form.useWatch 对辅助字段不可靠）
    const [endpointProtocol, setEndpointProtocol] = useState<string>();
    const [endpointMode, setEndpointMode] = useState<string>();
    const agentProtocolType = toProtocolType(endpointProtocol);

    // 统一协议类型
    const protocolType = connectionMode === 'agent' ? agentProtocolType : linkProtocolType;

    // 心跳包/注册包仅 Modbus/S7 的 TCP Server 模式使用；SL651 由报文遥测站地址识别。
    const linkMode = connectionMode === 'agent' ? endpointMode : selectedLink?.mode;
    const showPacketConfig =
        connectionMode !== 'agent' && linkMode === 'TCP Server' && protocolType !== 'SL651';

    const { data: protocolOptions, isLoading: protocolOptionsLoading } = useProtocolConfigOptions(
        protocolType ?? 'Modbus',
        {
            enabled: !!protocolType,
        }
    );

    // 编辑模式：端点数据异步加载后，同步协议信息到 state
    const agentEndpointId = Form.useWatch('agent_endpoint_id', form);
    useEffect(() => {
        if (!agentEndpointId || !agentEndpoints.length) return;
        const ep = agentEndpoints.find((e) => e.id === agentEndpointId);
        if (ep && !endpointProtocol) {
            setEndpointProtocol(ep.protocol);
            setEndpointMode(ep.mode);
        }
    }, [agentEndpoints, agentEndpointId, endpointProtocol]);

    type GroupSelectNode = { value: string; title: string; children?: GroupSelectNode[] };

    const groupTreeSelectData = useMemo(() => {
        const convert = (nodes: DeviceGroup.TreeItem[]): GroupSelectNode[] =>
            nodes.map((n) => ({
                value: n.id,
                title: n.name,
                children: n.children?.length ? convert(n.children) : undefined,
            }));
        return convert(groupTree);
    }, [groupTree]);

    const handleOpen = (isOpen: boolean) => {
        // 关闭动画结束后再清理表单，避免视觉闪烁
        if (!isOpen) {
            form.resetFields();
            setEndpointProtocol(undefined);
            setEndpointMode(undefined);
            return;
        }
        if (isOpen && editing) {
            const isAgentMode = false;
            form.setFieldsValue({
                id: editing.id,
                name: editing.name,
                device_code: editing.device_code,
                connection_mode: isAgentMode ? 'agent' : 'link',
                link_id: isAgentMode ? undefined : editing.link_id,
                target_id: editing.target_id,
                agent_id: editing.agent_id,
                agent_endpoint_id: editing.agent_endpoint_id,
                protocol_config_id: editing.protocol_config_id,
                status: editing.status,
                online_timeout: editing.online_timeout,
                remote_control: editing.remote_control ?? true,
                modbus_mode: editing.modbus_mode,
                slave_id: editing.slave_id ?? 1,
                timezone: editing.timezone ?? '+08:00',
                heartbeat: editing.heartbeat ?? { mode: 'OFF' },
                registration: editing.registration ?? { mode: 'OFF' },
                group_id: editing.group_id || undefined,
                remark: editing.remark,
            });
            // 编辑模式：协议信息会在 useEffect 中从 agentEndpoints 异步同步
        } else if (isOpen) {
            form.resetFields();
            form.setFieldsValue({
                connection_mode: 'link',
                status: 'enabled',
                remote_control: true,
                timezone: '+08:00',
                heartbeat: { mode: 'OFF' },
                registration: { mode: 'OFF' },
            });
            setEndpointProtocol(undefined);
            setEndpointMode(undefined);
        }
    };

    /** 链路变更时清除设备类型选择 */
    const handleLinkChange = () => {
        form.setFieldsValue({ target_id: undefined, protocol_config_id: undefined });
    };

    /** Agent 变更时清除端点和后续字段 */
    const handleAgentChange = () => {
        form.setFieldsValue({
            agent_endpoint_id: undefined,
            protocol_config_id: undefined,
            modbus_mode: undefined,
        });
        setEndpointProtocol(undefined);
        setEndpointMode(undefined);
    };

    /** 端点变更时同步写入协议信息并清除后续字段 */
    const handleEndpointChange = (endpointId: string) => {
        const ep = agentEndpoints.find((e) => e.id === endpointId);
        setEndpointProtocol(ep?.protocol);
        setEndpointMode(ep?.mode);
        form.setFieldsValue({
            protocol_config_id: undefined,
            modbus_mode: undefined,
        });
    };

    /** 连接方式变更时清除相关字段 */
    const handleConnectionModeChange = () => {
        form.setFieldsValue({
            link_id: undefined,
            target_id: undefined,
            agent_id: undefined,
            agent_endpoint_id: undefined,
            protocol_config_id: undefined,
            modbus_mode: undefined,
        });
        setEndpointProtocol(undefined);
        setEndpointMode(undefined);
    };

    return (
        <FormModal
            open={open}
            title={editing ? '编辑设备' : '新建设备'}
            onCancel={onCancel}
            onOk={() => form.submit()}
            confirmLoading={loading}
            destroyOnHidden
            width={520}
            afterOpenChange={handleOpen}
        >
            <Form<DeviceFormValues>
                form={form}
                layout="vertical"
                onFinish={(values) => {
                    if (!showPacketConfig) {
                        values.heartbeat = { mode: 'OFF' };
                        values.registration = { mode: 'OFF' };
                    }
                    // HEX 内容去除空格
                    if (values.heartbeat?.mode === 'HEX' && values.heartbeat?.content) {
                        values.heartbeat.content = values.heartbeat.content.replace(/\s/g, '');
                    }
                    if (values.registration?.mode === 'HEX' && values.registration?.content) {
                        values.registration.content = values.registration.content.replace(
                            /\s/g,
                            ''
                        );
                    }

                    onFinish(values);
                }}
            >
                <Form.Item name="id" hidden>
                    <Input />
                </Form.Item>

                {/* 基本信息 */}
                <Form.Item
                    label="设备名称"
                    name="name"
                    rules={[{ required: true, message: '请输入设备名称' }]}
                >
                    <Input placeholder="设备名称" maxLength={100} />
                </Form.Item>
                <Form.Item label="所属分组" name="group_id">
                    <TreeSelect
                        allowClear
                        treeData={groupTreeSelectData}
                        placeholder="不选则不归属任何分组"
                        treeDefaultExpandAll
                        fieldNames={{ label: 'title', value: 'value' }}
                    />
                </Form.Item>

                {/* 当前北桥 CRUD 仅绑定本地链路；Agent 在对应管理模块接入后启用。 */}
                <Form.Item
                    label="连接方式"
                    name="connection_mode"
                    rules={[{ required: true }]}
                    extra="当前阶段仅配置本地链路"
                >
                    <Radio.Group onChange={handleConnectionModeChange} disabled={!!editing}>
                        <Radio.Button value="link">本地链路</Radio.Button>
                    </Radio.Group>
                </Form.Item>

                {/* ===== 本地链路模式 ===== */}
                {connectionMode !== 'agent' && (
                    <Form.Item
                        label="关联链路"
                        name="link_id"
                        rules={[{ required: true, message: '请选择关联链路' }]}
                    >
                        <Select
                            placeholder="选择链路"
                            onChange={handleLinkChange}
                            disabled={!!editing}
                        >
                            {linkOptions.map((opt) => (
                                <Select.Option key={opt.id} value={opt.id}>
                                    {opt.name} ({opt.protocol} - {opt.mode}
                                    {opt.mode === 'TCP Server'
                                        ? ` - ${opt.ip}:${opt.port}`
                                        : ` - ${opt.targets?.length || 0} 个目标`}
                                    )
                                </Select.Option>
                            ))}
                        </Select>
                    </Form.Item>
                )}

                {connectionMode !== 'agent' && selectedLink?.mode === 'TCP Client' && (
                    <Form.Item
                        label="目标地址"
                        name="target_id"
                        rules={[{ required: true, message: '请选择该设备使用的目标地址' }]}
                    >
                        <Select placeholder="选择目标 IP:Port">
                            {(selectedLink.targets || []).map((target) => (
                                <Select.Option
                                    key={target.id}
                                    value={target.id}
                                    disabled={
                                        target.status !== 'enabled' &&
                                        target.id !== editing?.target_id
                                    }
                                >
                                    {target.name} ({target.ip}:{target.port})
                                    {target.status === 'disabled' ? ' [已禁用]' : ''}
                                </Select.Option>
                            ))}
                        </Select>
                    </Form.Item>
                )}

                {/* ===== 采集 Agent 模式 ===== */}
                {connectionMode === 'agent' && (
                    <>
                        <Form.Item
                            label="采集 Agent"
                            name="agent_id"
                            rules={[{ required: true, message: '请选择采集 Agent' }]}
                        >
                            <Select
                                placeholder="选择 Agent"
                                onChange={handleAgentChange}
                                disabled={!!editing}
                            >
                                {agentOptions.map((agent) => (
                                    <Select.Option key={agent.id} value={agent.id}>
                                        {agent.name} ({agent.code})
                                        {agent.is_online ? '' : ' [离线]'}
                                    </Select.Option>
                                ))}
                            </Select>
                        </Form.Item>

                        {agentId && (
                            <Form.Item
                                label="接入端点"
                                name="agent_endpoint_id"
                                rules={[{ required: true, message: '请选择接入端点' }]}
                            >
                                <Select
                                    placeholder={
                                        agentEndpoints.length
                                            ? '选择端点'
                                            : '该 Agent 暂无端点，请先在 Agent 页面创建'
                                    }
                                    onChange={handleEndpointChange}
                                    disabled={!!editing}
                                >
                                    {agentEndpoints.map((ep) => (
                                        <Select.Option key={ep.id} value={ep.id}>
                                            {ep.name}
                                            {ep.transport === 'serial'
                                                ? ` (${ep.channel} · ${ep.baud_rate || ''} bps)`
                                                : ` (${ep.mode} · ${[ep.ip, ep.port].filter(Boolean).join(':')})`}
                                        </Select.Option>
                                    ))}
                                </Select>
                            </Form.Item>
                        )}
                    </>
                )}

                {/* 设备类型（两种模式共用） */}
                <Form.Item
                    label="设备类型"
                    name="protocol_config_id"
                    rules={[{ required: true, message: '请选择设备类型' }]}
                >
                    <Select
                        placeholder={
                            protocolType
                                ? '选择设备类型'
                                : connectionMode === 'agent'
                                  ? '请先选择端点'
                                  : '请先选择链路'
                        }
                        disabled={!!editing || !protocolType}
                        loading={protocolOptionsLoading}
                        notFoundContent={protocolOptionsLoading ? '加载中...' : '暂无数据'}
                    >
                        {(protocolOptions?.list || []).map((opt) => (
                            <Select.Option key={opt.id} value={opt.id}>
                                {opt.name}
                            </Select.Option>
                        ))}
                    </Select>
                </Form.Item>

                {/* Modbus 模式选择（Agent 模式下由端点 transport 自动决定，无需选择） */}
                {protocolType === 'Modbus' && connectionMode !== 'agent' && (
                    <Form.Item
                        label="Modbus 模式"
                        name="modbus_mode"
                        rules={[{ required: true, message: '请选择 Modbus 模式' }]}
                        extra={
                            linkMode === 'TCP Server'
                                ? 'TCP Server：选 RTU 表示 DTU 串口透传，选 TCP 表示设备直接以 ModbusTCP 连入'
                                : 'TCP Client：需指定 Modbus 通信模式'
                        }
                    >
                        <Select placeholder="选择 Modbus 模式">
                            <Select.Option value="TCP">Modbus TCP</Select.Option>
                            <Select.Option value="RTU">Modbus RTU</Select.Option>
                        </Select>
                    </Form.Item>
                )}

                {protocolType === 'Modbus' && (
                    <Form.Item
                        label="从站地址 (Slave ID)"
                        name="slave_id"
                        rules={[{ required: true, message: '请输入从站地址' }]}
                        extra="Modbus 从站地址，范围 1-247"
                    >
                        <InputNumber min={1} max={247} placeholder="默认 1" className="!w-full" />
                    </Form.Item>
                )}
                <Form.Item
                    label="设备编码"
                    name="device_code"
                    rules={[
                        { required: true, message: '请输入设备编码' },
                        protocolType === 'SL651'
                            ? {
                                  pattern: /^\d{1,10}$/,
                                  message: 'SL651 设备编码必须是 1-10 位数字遥测站地址',
                              }
                            : {
                                  pattern: /^[A-Za-z0-9]+$/,
                                  message: '设备编码只能包含字母和数字',
                              },
                    ]}
                    extra={
                        protocolType === 'SL651'
                            ? '遥测站地址，用于从 SL651 报文中识别设备'
                            : '每台设备必填且唯一，用于跨协议统一识别'
                    }
                >
                    <Input
                        placeholder={protocolType === 'SL651' ? '如: 12345678' : '如: DEVICE001'}
                        maxLength={protocolType === 'SL651' ? 10 : 100}
                    />
                </Form.Item>
                <Form.Item label="状态" name="status" rules={[{ required: true }]}>
                    <Select>
                        <Select.Option value="enabled">启用</Select.Option>
                        <Select.Option value="disabled">禁用</Select.Option>
                    </Select>
                </Form.Item>

                {/* 高级配置 */}
                <Collapse
                    ghost
                    className="!-mx-6 !mb-0"
                    items={[
                        {
                            key: 'advanced',
                            label: '高级配置',
                            children: (
                                <>
                                    {protocolType === 'SL651' && (
                                        <Form.Item
                                            label="在线超时时间"
                                            name="online_timeout"
                                            extra="设备无心跳或数据上报超过此时间视为离线，单位：秒"
                                        >
                                            <InputNumber
                                                placeholder="默认 300 秒（5分钟）"
                                                min={1}
                                                className="!w-full"
                                            />
                                        </Form.Item>
                                    )}
                                    {protocolType === 'SL651' && (
                                        <Form.Item
                                            label="允许远控"
                                            name="remote_control"
                                            valuePropName="checked"
                                            extra="关闭后将禁止对该设备下发指令"
                                        >
                                            <Switch checkedChildren="是" unCheckedChildren="否" />
                                        </Form.Item>
                                    )}
                                    <Form.Item
                                        label="设备时区"
                                        name="timezone"
                                        rules={[{ required: true, message: '请选择设备时区' }]}
                                        extra="仅用于解析设备报文时间；数据库始终存储 UTC"
                                    >
                                        <Select showSearch options={TIMEZONE_OPTIONS} />
                                    </Form.Item>
                                    {showPacketConfig && (
                                        <>
                                            <Form.Item
                                                label="心跳包模式"
                                                name={['heartbeat', 'mode']}
                                            >
                                                <Select>
                                                    <Select.Option value="OFF">关闭</Select.Option>
                                                    <Select.Option value="HEX">HEX</Select.Option>
                                                    <Select.Option value="ASCII">
                                                        ASCII
                                                    </Select.Option>
                                                </Select>
                                            </Form.Item>
                                            <Form.Item
                                                noStyle
                                                dependencies={[['heartbeat', 'mode']]}
                                            >
                                                {({ getFieldValue }) => {
                                                    const hbMode = getFieldValue([
                                                        'heartbeat',
                                                        'mode',
                                                    ]);
                                                    if (hbMode === 'OFF' || !hbMode) return null;
                                                    return (
                                                        <Form.Item
                                                            label="心跳包内容"
                                                            name={['heartbeat', 'content']}
                                                            rules={
                                                                hbMode === 'HEX'
                                                                    ? hexContentRules
                                                                    : [
                                                                          {
                                                                              required: true,
                                                                              message:
                                                                                  '请输入心跳包内容',
                                                                          },
                                                                      ]
                                                            }
                                                            extra={
                                                                hbMode === 'HEX'
                                                                    ? '十六进制字符串，如: AA BB CC DD（空格会自动去除）'
                                                                    : 'ASCII 字符串，支持 \\r \\n 转义'
                                                            }
                                                        >
                                                            <Input
                                                                placeholder={
                                                                    hbMode === 'HEX'
                                                                        ? 'AA BB CC DD'
                                                                        : 'HELLO\\r\\n'
                                                                }
                                                            />
                                                        </Form.Item>
                                                    );
                                                }}
                                            </Form.Item>
                                            <Form.Item
                                                label="注册包模式"
                                                name={['registration', 'mode']}
                                            >
                                                <Select>
                                                    <Select.Option value="OFF">关闭</Select.Option>
                                                    <Select.Option value="HEX">HEX</Select.Option>
                                                    <Select.Option value="ASCII">
                                                        ASCII
                                                    </Select.Option>
                                                </Select>
                                            </Form.Item>
                                            <Form.Item
                                                noStyle
                                                dependencies={[['registration', 'mode']]}
                                            >
                                                {({ getFieldValue }) => {
                                                    const regMode = getFieldValue([
                                                        'registration',
                                                        'mode',
                                                    ]);
                                                    if (regMode === 'OFF' || !regMode) return null;
                                                    return (
                                                        <Form.Item
                                                            label="注册包内容"
                                                            name={['registration', 'content']}
                                                            rules={
                                                                regMode === 'HEX'
                                                                    ? hexContentRules
                                                                    : [
                                                                          {
                                                                              required: true,
                                                                              message:
                                                                                  '请输入注册包内容',
                                                                          },
                                                                      ]
                                                            }
                                                            extra={
                                                                regMode === 'HEX'
                                                                    ? '十六进制字符串，如: AA BB CC DD（空格会自动去除）'
                                                                    : 'ASCII 字符串，支持 \\r \\n 转义'
                                                            }
                                                        >
                                                            <Input
                                                                placeholder={
                                                                    regMode === 'HEX'
                                                                        ? 'AA BB CC DD'
                                                                        : 'HELLO\\r\\n'
                                                                }
                                                            />
                                                        </Form.Item>
                                                    );
                                                }}
                                            </Form.Item>
                                        </>
                                    )}
                                    <Form.Item label="备注" name="remark">
                                        <Input.TextArea rows={3} placeholder="备注信息" />
                                    </Form.Item>
                                </>
                            ),
                        },
                    ]}
                />
            </Form>
        </FormModal>
    );
};

export default DeviceFormModal;

export type { DeviceFormValues };
