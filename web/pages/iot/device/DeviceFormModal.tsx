/**
 * 设备表单弹窗组件
 */

import { Collapse, Form, Input, InputNumber, Radio, Select, Switch, TreeSelect } from 'antd';
import type { RuleObject } from 'antd/es/form';
import { useMemo } from 'react';
import { FormModal } from '@/components/FormModal';

import { useEdgeDetail, useEdgeList } from '../edge-node/edge-node.service';
import type { Link } from '../link/link.types';
import { useProtocolConfigOptions } from '../protocol/protocol.service';
import type { Protocol } from '../protocol/protocol.types';
import { useDeviceGroupTree } from './device.service';
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
type ConnectionMode = 'link' | 'edge';

interface DeviceFormValues {
    id?: string;
    name: string;
    device_code: string;
    connection_mode: ConnectionMode;
    link_id?: string;
    target_id?: string;
    edge_node_id?: string;
    edge_protocol?: Protocol.Type;
    edge_transport?: Device.EdgeTransport;
    edge_interface?: string;
    edge_mode?: Device.EdgeMode;
    edge_ip?: string;
    edge_port?: number;
    serial_baud_rate?: number;
    serial_data_bits?: number;
    serial_stop_bits?: number;
    serial_parity?: Device.SerialParity;
    serial_rs485?: boolean;

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
    editing: Device.RealTimeData | null;
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
    const { data: edgeListData, isLoading: edgeListLoading } = useEdgeList(
        { page: 1, pageSize: 100, status: 'approved' },
        open
    );

    const connectionMode = Form.useWatch('connection_mode', form);
    const linkId = Form.useWatch('link_id', form);
    const edgeNodeId = Form.useWatch('edge_node_id', form);
    const edgeProtocol = Form.useWatch('edge_protocol', form);
    const edgeTransport = Form.useWatch('edge_transport', form);
    const edgeMode = Form.useWatch('edge_mode', form);
    const { data: edgeNode, isLoading: edgeNodeLoading } = useEdgeDetail(
        connectionMode === 'edge' ? edgeNodeId : undefined
    );
    const edgeInterfacesWithIp = useMemo(
        () =>
            (edgeNode?.interfaces ?? []).filter(
                (network) => !network.bridge && network.ipv4?.trim()
            ),
        [edgeNode?.interfaces]
    );

    // 本地链路模式：协议类型从选中链路推导
    const selectedLink = linkOptions.find((opt) => opt.id === linkId);
    const linkProtocolType = toProtocolType(selectedLink?.protocol);

    // 统一协议类型
    const protocolType = connectionMode === 'edge' ? edgeProtocol : linkProtocolType;

    // 心跳包/注册包仅 Modbus/S7 的 TCP Server 模式使用。
    const linkMode = connectionMode === 'edge' ? edgeMode : selectedLink?.endpoint.mode;
    const showPacketConfig = linkMode === 'TCP Server' && protocolType !== 'SL651';

    const { data: protocolOptions, isLoading: protocolOptionsLoading } = useProtocolConfigOptions(
        protocolType ?? 'Modbus',
        {
            enabled: !!protocolType,
        }
    );

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
            return;
        }
        if (isOpen && editing) {
            const isEdgeMode = Boolean(editing.edge_node_id);
            form.setFieldsValue({
                id: editing.id,
                name: editing.name,
                device_code: editing.device_code,
                connection_mode: isEdgeMode ? 'edge' : 'link',
                link_id: isEdgeMode ? undefined : editing.link_id,
                target_id: editing.target_id,
                edge_node_id: editing.edge_node_id,
                edge_protocol: editing.protocol_type,
                edge_transport: editing.edge_transport,
                edge_interface: editing.edge_interface,
                edge_mode: editing.edge_mode,
                edge_ip: editing.edge_ip,
                edge_port: editing.edge_port,
                serial_baud_rate: editing.serial_baud_rate,
                serial_data_bits: editing.serial_data_bits,
                serial_stop_bits: editing.serial_stop_bits,
                serial_parity: editing.serial_parity,
                serial_rs485: editing.serial_rs485,
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
        }
    };

    /** 链路变更时清除设备类型选择 */
    const handleLinkChange = () => {
        form.setFieldsValue({ target_id: undefined, protocol_config_id: undefined });
    };

    /** 边缘节点变更时清除其接口和后续字段。 */
    const handleEdgeNodeChange = () => {
        form.setFieldsValue({
            edge_interface: undefined,
            protocol_config_id: undefined,
            modbus_mode: undefined,
        });
    };

    /** 边缘协议变更时清除端点约束和设备类型。 */
    const handleEdgeProtocolChange = (protocol: Protocol.Type) => {
        form.setFieldsValue({
            protocol_config_id: undefined,
            modbus_mode: undefined,
            edge_transport: protocol === 'S7' ? 'tcp' : undefined,
            edge_mode: protocol === 'S7' ? 'TCP Client' : undefined,
            edge_interface: undefined,
        });
    };

    const handleEdgeTransportChange = (transport: Device.EdgeTransport) => {
        form.setFieldsValue({
            edge_interface: undefined,
            edge_mode: transport === 'tcp' ? form.getFieldValue('edge_mode') : undefined,
            edge_ip: transport === 'tcp' ? form.getFieldValue('edge_ip') : undefined,
            edge_port: transport === 'tcp' ? form.getFieldValue('edge_port') : undefined,
            serial_baud_rate: transport === 'serial' ? 9600 : undefined,
            serial_data_bits: transport === 'serial' ? 8 : undefined,
            serial_stop_bits: transport === 'serial' ? 1 : undefined,
            serial_parity: transport === 'serial' ? 'none' : undefined,
            serial_rs485: transport === 'serial',
        });
    };

    const handleEdgeInterfaceChange = (interfaceName: string) => {
        if (edgeTransport !== 'tcp' || edgeMode !== 'TCP Server') return;
        const item = edgeNode?.interfaces?.find((candidate) => candidate.name === interfaceName);
        if (item?.ipv4) form.setFieldValue('edge_ip', item.ipv4);
    };

    const handleEdgeModeChange = (mode: Device.EdgeMode) => {
        if (mode !== 'TCP Server') return;
        const interfaceName = form.getFieldValue('edge_interface');
        const item = edgeNode?.interfaces?.find((candidate) => candidate.name === interfaceName);
        if (item?.ipv4) form.setFieldValue('edge_ip', item.ipv4);
    };

    /** 连接方式变更时清除相关字段 */
    const handleConnectionModeChange = () => {
        form.setFieldsValue({
            link_id: undefined,
            target_id: undefined,
            edge_node_id: undefined,
            edge_protocol: undefined,
            edge_transport: undefined,
            edge_interface: undefined,
            edge_mode: undefined,
            edge_ip: undefined,
            edge_port: undefined,
            protocol_config_id: undefined,
            modbus_mode: undefined,
        });
    };

    return (
        <FormModal
            open={open}
            title={editing ? '编辑设备' : '新建设备'}
            onCancel={onCancel}
            onOk={() => form.submit()}
            confirmLoading={loading}
            destroyOnHidden
            width={720}
            afterOpenChange={handleOpen}
        >
            <Form<DeviceFormValues>
                form={form}
                layout="vertical"
                onFinish={(values) => {
                    if (connectionMode === 'edge' && protocolType === 'Modbus') {
                        values.modbus_mode = edgeTransport === 'serial' ? 'RTU' : 'TCP';
                    }
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

                <Form.Item
                    label="连接方式"
                    name="connection_mode"
                    rules={[{ required: true }]}
                    extra="边缘节点模式会把设备和要素配置下发到选定 OpenWrt 节点"
                >
                    <Radio.Group onChange={handleConnectionModeChange} disabled={!!editing}>
                        <Radio.Button value="link">本地链路</Radio.Button>
                        <Radio.Button value="edge">边缘节点</Radio.Button>
                    </Radio.Group>
                </Form.Item>

                {/* ===== 本地链路模式 ===== */}
                {connectionMode === 'link' && (
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
                                    {opt.name} ({opt.protocol} - {opt.endpoint.mode}
                                    {opt.endpoint.mode === 'TCP Server'
                                        ? ` - ${opt.endpoint.ip}:${opt.endpoint.port}`
                                        : ` - ${opt.endpoint.targets?.length || 0} 个目标`}
                                    )
                                </Select.Option>
                            ))}
                        </Select>
                    </Form.Item>
                )}

                {connectionMode === 'link' && selectedLink?.endpoint.mode === 'TCP Client' && (
                    <Form.Item
                        label="目标地址"
                        name="target_id"
                        rules={[{ required: true, message: '请选择该设备使用的目标地址' }]}
                    >
                        <Select placeholder="选择目标 IP:Port">
                            {(selectedLink.endpoint.targets || []).map((target) => (
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

                {/* ===== 边缘节点采集模式 ===== */}
                {connectionMode === 'edge' && (
                    <>
                        <Form.Item
                            label="所属边缘节点"
                            name="edge_node_id"
                            rules={[{ required: true, message: '请选择已批准的边缘节点' }]}
                        >
                            <Select
                                placeholder="选择边缘节点（IMEI）"
                                onChange={handleEdgeNodeChange}
                                disabled={!!editing}
                                loading={edgeListLoading}
                                showSearch
                                optionFilterProp="label"
                            >
                                {(edgeListData?.list ?? []).map((node) => (
                                    <Select.Option
                                        key={node.id}
                                        value={node.id}
                                        label={`${node.name || node.hostname || node.imei} ${node.imei}`}
                                        disabled={!node.capability.deviceConfig}
                                    >
                                        {node.name || node.hostname || '未命名节点'} ({node.imei})
                                        {!node.capability.deviceConfig
                                            ? ' [需升级 edgenode]'
                                            : node.status.online
                                              ? ''
                                              : ' [离线]'}
                                    </Select.Option>
                                ))}
                            </Select>
                        </Form.Item>

                        <div className="grid grid-cols-1 gap-x-4 md:grid-cols-2">
                            <Form.Item
                                label="协议"
                                name="edge_protocol"
                                rules={[{ required: true, message: '请选择设备协议' }]}
                            >
                                <Select
                                    placeholder="选择协议"
                                    onChange={handleEdgeProtocolChange}
                                    disabled={!!editing}
                                >
                                    <Select.Option value="Modbus">Modbus</Select.Option>
                                    <Select.Option value="S7">S7</Select.Option>
                                </Select>
                            </Form.Item>
                            <Form.Item
                                label="传输方式"
                                name="edge_transport"
                                rules={[{ required: true, message: '请选择传输方式' }]}
                            >
                                <Select
                                    placeholder="选择串口或 TCP"
                                    onChange={handleEdgeTransportChange}
                                    disabled={!edgeNodeId || edgeProtocol === 'S7'}
                                >
                                    {edgeProtocol !== 'S7' && (
                                        <Select.Option value="serial">串口</Select.Option>
                                    )}
                                    <Select.Option value="tcp">TCP</Select.Option>
                                </Select>
                            </Form.Item>
                        </div>

                        {edgeTransport && (
                            <Form.Item
                                label={edgeTransport === 'serial' ? '节点串口' : '节点网口'}
                                name="edge_interface"
                                rules={[{ required: true, message: '请选择节点已上报的接口' }]}
                            >
                                <Select
                                    placeholder={
                                        edgeNodeLoading
                                            ? '正在读取节点接口'
                                            : '选择节点已上报的接口'
                                    }
                                    loading={edgeNodeLoading}
                                    onChange={handleEdgeInterfaceChange}
                                >
                                    {edgeTransport === 'serial'
                                        ? (edgeNode?.serialPorts ?? []).map((serial) => (
                                              <Select.Option
                                                  key={serial.path}
                                                  value={serial.path}
                                                  disabled={!serial.available}
                                              >
                                                  {serial.displayName || serial.path} ({serial.path}
                                                  ){serial.rs485 ? ' · RS485' : ''}
                                                  {serial.available ? '' : ' [不可用]'}
                                              </Select.Option>
                                          ))
                                        : edgeInterfacesWithIp.map((network) => (
                                              <Select.Option
                                                  key={network.name}
                                                  value={network.name}
                                              >
                                                  {network.displayName || network.name}
                                                  {` · ${network.ipv4}`}
                                                  {network.up ? '' : ' [未连接]'}
                                              </Select.Option>
                                          ))}
                                </Select>
                            </Form.Item>
                        )}

                        {edgeTransport === 'tcp' && (
                            <div className="grid grid-cols-1 gap-x-4 md:grid-cols-3">
                                <Form.Item
                                    label="TCP 模式"
                                    name="edge_mode"
                                    rules={[{ required: true, message: '请选择 TCP 模式' }]}
                                >
                                    <Select onChange={handleEdgeModeChange}>
                                        <Select.Option value="TCP Client">TCP Client</Select.Option>
                                        {edgeProtocol !== 'S7' && (
                                            <Select.Option value="TCP Server">
                                                TCP Server
                                            </Select.Option>
                                        )}
                                    </Select>
                                </Form.Item>
                                <Form.Item
                                    className="md:col-span-1"
                                    label={edgeMode === 'TCP Server' ? '监听 IPv4' : '目标 IPv4'}
                                    name="edge_ip"
                                    rules={[{ required: true, message: '请输入 IPv4 地址' }]}
                                >
                                    <Input placeholder="192.168.1.10" />
                                </Form.Item>
                                <Form.Item
                                    label={edgeMode === 'TCP Server' ? '监听端口' : '目标端口'}
                                    name="edge_port"
                                    rules={[{ required: true, message: '请输入端口' }]}
                                >
                                    <InputNumber className="w-full" min={1} max={65535} />
                                </Form.Item>
                            </div>
                        )}

                        {edgeTransport === 'serial' && (
                            <div className="grid grid-cols-1 gap-x-4 md:grid-cols-3">
                                <Form.Item label="波特率" name="serial_baud_rate">
                                    <InputNumber className="w-full" min={300} max={4_000_000} />
                                </Form.Item>
                                <Form.Item label="数据位" name="serial_data_bits">
                                    <Select options={[5, 6, 7, 8].map((value) => ({ value }))} />
                                </Form.Item>
                                <Form.Item label="停止位" name="serial_stop_bits">
                                    <Select options={[1, 2].map((value) => ({ value }))} />
                                </Form.Item>
                                <Form.Item label="校验位" name="serial_parity">
                                    <Select
                                        options={[
                                            { value: 'none', label: '无' },
                                            { value: 'even', label: '偶校验' },
                                            { value: 'odd', label: '奇校验' },
                                        ]}
                                    />
                                </Form.Item>
                                <Form.Item
                                    label="RS485"
                                    name="serial_rs485"
                                    valuePropName="checked"
                                >
                                    <Switch />
                                </Form.Item>
                            </div>
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
                                : connectionMode === 'edge'
                                  ? '请先选择边缘协议'
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

                {/* 边缘 Modbus 模式由串口/TCP 自动决定。 */}
                {protocolType === 'Modbus' && connectionMode === 'link' && (
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
