import { Form, Input, InputNumber, Select, Switch } from 'antd';
import { FormModal } from '@/components/FormModal';
import { useLiveQuery } from '@/hooks/useLiveQuery';
import { getRevisions } from '../protocol/protocol.client';
import { useProtocolConfigOptions } from '../protocol/protocol.service';
import type { Link } from '../link/link.types';
import type { Device } from './device.types';
import { useDeviceGroupTree } from './device.service';
import type { DeviceGroup } from './device-group.types';

export type DeviceFormValues = Device.CreateDto & { id?: string };
interface Props {
    open: boolean; editing: Device.RealTimeData | null; loading: boolean;
    linkOptions: Link.Option[]; onCancel: () => void; onFinish: (values: DeviceFormValues) => void;
}
export default function DeviceFormModal({ open, editing, loading, linkOptions, onCancel, onFinish }: Props) {
    const [form] = Form.useForm<DeviceFormValues>();
    const linkId = Form.useWatch('link_id', form);
    const modelId = Form.useWatch('protocol_config_id', form);
    const channel = linkOptions.find((value) => value.id === linkId);
    const protocol = channel?.protocol;
    const { data: models } = useProtocolConfigOptions(protocol ?? 'Modbus', { enabled: open && !!protocol });
    const { data: revisions = [] } = useLiveQuery({ queryKey: ['protocol-configs', 'revisions', modelId],
        queryFn: () => getRevisions(modelId ?? ''), enabled: open && !!modelId });
    const { data: groups = [] } = useDeviceGroupTree();
    const flatten = (nodes: DeviceGroup.TreeItem[]): { value: string; label: string }[] =>
        nodes.flatMap((node) => [{ value: node.id, label: node.name }, ...flatten(node.children ?? [])]);
    const packet = (name: 'heartbeat' | 'registration', label: string) => <>
        <Form.Item label={label} name={[name, 'mode']}><Select options={['OFF','HEX','ASCII'].map(value => ({value,label:value}))} /></Form.Item>
        <Form.Item label={`${label}内容`} name={[name, 'content']}><Input /></Form.Item>
    </>;
    return <FormModal open={open} title={editing ? '编辑设备' : '新增设备'} onCancel={onCancel}
        onOk={() => form.submit()} confirmLoading={loading} destroyOnHidden width={640}
        afterOpenChange={(visible) => { if (visible) { form.resetFields(); form.setFieldsValue(editing ? {
            ...editing, heartbeat: editing.heartbeat ?? {mode:'OFF'}, registration: editing.registration ?? {mode:'OFF'},
        } : {status:'enabled',online_timeout:300,remote_control:true,timezone:'+08:00',slave_id:1,
            heartbeat:{mode:'OFF'},registration:{mode:'OFF'}}); } }}>
        <Form form={form} layout="vertical" onFinish={(values) => {
            if (channel?.execution === 'edge') {
                values.registration = {mode:'OFF'};
                values.heartbeat = {mode:'OFF'};
                if (protocol === 'Modbus') values.modbus_mode = channel.endpoint.transport === 'serial' ? 'RTU' : 'TCP';
            }
            if (protocol === 'SL651') values.device_code = values.device_code.padStart(10,'0');
            onFinish(values);
        }}>
            <Form.Item name="name" label="设备名称" rules={[{required:true}]}><Input maxLength={100} /></Form.Item>
            <Form.Item name="device_code" label="设备编码" rules={[{required:true}]}><Input maxLength={100} /></Form.Item>
            <Form.Item name="link_id" label="物理通道" rules={[{required:true,message:'请先在链路管理中创建通道'}]}>
                <Select showSearch optionFilterProp="label" options={linkOptions.map(c => ({value:c.id,label:`${c.name} · ${c.execution === 'edge' ? '边缘' : '平台'} · ${c.protocol}`}))}
                    onChange={() => form.setFieldsValue({protocol_config_id:undefined,protocol_revision:undefined,target_id:undefined})} />
            </Form.Item>
            {channel?.execution !== 'edge' && channel?.endpoint.mode === 'TCP Client' &&
                <Form.Item name="target_id" label="目标地址" rules={[{required:true}]}><Select options={channel.endpoint.targets.map(t => ({value:t.id,label:`${t.name} · ${t.ip}:${t.port}`}))} /></Form.Item>}
            <Form.Item name="protocol_config_id" label="设备类型" rules={[{required:true}]}><Select disabled={!protocol}
                options={models?.list.map(m => ({value:m.id,label:m.name}))}
                onChange={() => form.setFieldValue('protocol_revision',undefined)} /></Form.Item>
            <Form.Item name="protocol_revision" label="已发布版本" rules={[{required:true}]}><Select disabled={!modelId}
                options={revisions.map(r => ({value:r.revision,label:`v${r.revision} · ${r.name}`}))} /></Form.Item>
            {protocol === 'Modbus' && <>
                <Form.Item name="slave_id" label="从站地址" rules={[{required:true}]}><InputNumber min={1} max={247} /></Form.Item>
                {channel?.execution !== 'edge' && <Form.Item name="modbus_mode" label="Modbus 模式" rules={[{required:true}]}><Select options={['TCP','RTU'].map(value => ({value,label:value}))} /></Form.Item>}
            </>}
            {channel?.execution !== 'edge' && channel?.endpoint.mode === 'TCP Server' && protocol !== 'SL651' && <>{packet('heartbeat','心跳包')}{packet('registration','注册包')}</>}
            <Form.Item name="group_id" label="设备分组"><Select allowClear options={flatten(groups)} /></Form.Item>
            <Form.Item name="status" label="状态"><Select options={[{value:'enabled',label:'启用'},{value:'disabled',label:'停用'}]} /></Form.Item>
            <Form.Item name="online_timeout" label="离线超时（秒）"><InputNumber min={1} max={86400} /></Form.Item>
            <Form.Item name="remote_control" label="允许远控" valuePropName="checked"><Switch /></Form.Item>
            <Form.Item name="timezone" label="设备时区"><Input placeholder="+08:00" /></Form.Item>
            <Form.Item name="remark" label="备注"><Input.TextArea /></Form.Item>
        </Form>
    </FormModal>;
}
