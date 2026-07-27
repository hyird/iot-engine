import { useQueryClient } from '@tanstack/react-query';
import {
    App,
    Button,
    Divider,
    Input,
    Modal,
    Result,
    Select,
    Skeleton,
    Space,
    Table,
    Tag,
} from 'antd';
import type { ColumnsType, TablePaginationConfig } from 'antd/es/table';
import { useState } from 'react';
import { PageContainer } from '@/components/PageContainer';
import { StatusTag } from '@/components/StatusTag';
import { useDebounceFn } from '@/hooks/useDebounceFn';
import { usePermission } from '@/hooks/usePermission';
import {
    alertApi,
    useAlertAcknowledge,
    useAlertApplyTemplate,
    useAlertBatchAcknowledge,
    useAlertRecordList,
    useAlertRuleBatchDelete,
    useAlertRuleDelete,
    useAlertRuleList,
    useAlertRuleSave,
    useAlertStats,
    useAlertTemplateDelete,
    useAlertTemplateList,
    useAlertTemplateSave,
    useDeviceOptions,
    alertKeys,
} from './alert.service';
import type { Alert } from './alert.types';
import { formatDateTime } from '@/utils/dateTime';
import { AlertRuleFormModal } from './AlertRuleFormModal';
import { AlertTemplateFormModal } from './AlertTemplateFormModal';

const { Search } = Input;
const DATE_TIME_COLUMN_WIDTH = 180;

const SEVERITY_OPTIONS = [
    { label: '全部级别', value: '' },
    { label: '严重', value: 'critical' },
    { label: '警告', value: 'warning' },
    { label: '信息', value: 'info' },
];

const STATUS_OPTIONS = [
    { label: '全部状态', value: '' },
    { label: '活跃', value: 'active' },
    { label: '已确认', value: 'acknowledged' },
    { label: '已恢复', value: 'resolved' },
];

const SEVERITY_COLORS: Record<string, string> = {
    critical: 'red',
    warning: 'orange',
    info: 'blue',
};

const STATUS_COLORS: Record<string, string> = {
    active: 'error',
    acknowledged: 'warning',
    resolved: 'success',
};

const STATUS_LABELS: Record<string, string> = {
    active: '活跃',
    acknowledged: '已确认',
    resolved: '已恢复',
};

const SEVERITY_LABELS: Record<string, string> = {
    critical: '严重',
    warning: '警告',
    info: '信息',
};

// ==================== 规则配置弹窗（规则 + 模板上下布局）====================

function RuleConfigModal({ open, onClose }: { open: boolean; onClose: () => void }) {
    // ---- 规则状态 ----
    const [ruleKeyword, setRuleKeyword] = useState('');
    const [ruleSeverity, setRuleSeverity] = useState('');
    const [rulePagination, setRulePagination] = useState({ page: 1, pageSize: 5 });
    const [ruleFormVisible, setRuleFormVisible] = useState(false);
    const [ruleEditing, setRuleEditing] = useState<Alert.RuleItem | null>(null);
    const [ruleSelectedKeys, setRuleSelectedKeys] = useState<string[]>([]);

    // ---- 模板状态 ----
    const [tplPagination, setTplPagination] = useState({ page: 1, pageSize: 5 });
    const [tplFormVisible, setTplFormVisible] = useState(false);
    const [tplEditing, setTplEditing] = useState<Alert.TemplateItem | null>(null);
    const [tplEditingDetail, setTplEditingDetail] = useState<Alert.TemplateDetail | null>(null);
    const [applyingTemplate, setApplyingTemplate] = useState<Alert.TemplateItem | null>(null);
    const [selectedDeviceIds, setSelectedDeviceIds] = useState<string[]>([]);

    const { modal } = App.useApp();
    const queryClient = useQueryClient();

    const canAdd = usePermission('iot:alert:add');
    const canEdit = usePermission('iot:alert:edit');
    const canDelete = usePermission('iot:alert:delete');

    // ---- 规则数据 ----
    const doRuleSearch = (value: string) => {
        setRuleKeyword(value);
        setRulePagination((prev) => ({ ...prev, page: 1 }));
    };
    const { run: debouncedRuleSearch } = useDebounceFn(doRuleSearch, 300);

    const { data: rulePage, isLoading: ruleLoading } = useAlertRuleList({
        page: rulePagination.page,
        pageSize: rulePagination.pageSize,
        keyword: ruleKeyword || undefined,
        severity: ruleSeverity || undefined,
    });

    const ruleSaveMutation = useAlertRuleSave();
    const ruleDeleteMutation = useAlertRuleDelete();
    const ruleBatchDeleteMutation = useAlertRuleBatchDelete();

    // ---- 模板数据 ----
    const { data: templatePage, isLoading: tplLoading } = useAlertTemplateList({
        page: tplPagination.page,
        pageSize: tplPagination.pageSize,
    });

    const tplSaveMutation = useAlertTemplateSave();
    const tplDeleteMutation = useAlertTemplateDelete();
    const applyMutation = useAlertApplyTemplate();

    // ---- 规则操作 ----
    const openRuleCreate = () => {
        setRuleEditing(null);
        setRuleFormVisible(true);
    };
    const openRuleEdit = (record: Alert.RuleItem) => {
        setRuleEditing(record);
        setRuleFormVisible(true);
    };
    const onRuleDelete = (record: Alert.RuleItem) => {
        modal.confirm({
            title: `确认删除规则「${record.name}」吗？`,
            content: '删除后该规则将停止告警检测。此操作不可撤销。',
            okText: '确定删除',
            okButtonProps: { danger: true },
            onOk: () => ruleDeleteMutation.mutate(record.id),
        });
    };
    const onRuleBatchDelete = () => {
        if (ruleSelectedKeys.length === 0) return;
        modal.confirm({
            title: '批量删除',
            content: `确认删除选中的 ${ruleSelectedKeys.length} 条规则？此操作不可撤销。`,
            okText: '确定删除',
            okButtonProps: { danger: true },
            onOk: () =>
                ruleBatchDeleteMutation.mutate(ruleSelectedKeys, {
                    onSuccess: () => setRuleSelectedKeys([]),
                }),
        });
    };

    // ---- 模板操作 ----
    const openTplCreate = () => {
        setTplEditing(null);
        setTplEditingDetail(null);
        setTplFormVisible(true);
    };
    const openTplEdit = async (record: Alert.TemplateItem) => {
        try {
            const detail = await alertApi.getTemplateDetail(record.id);
            setTplEditing(record);
            setTplEditingDetail(detail);
            setTplFormVisible(true);
        } catch {
            // 错误由 axios 拦截器处理
        }
    };
    const onTplDelete = (record: Alert.TemplateItem) => {
        modal.confirm({
            title: `确认删除模板「${record.name}」吗？`,
            content: '此操作不可撤销。',
            okText: '确定删除',
            okButtonProps: { danger: true },
            onOk: () => tplDeleteMutation.mutate(record.id),
        });
    };
    const openApplyModal = (record: Alert.TemplateItem) => {
        setApplyingTemplate(record);
        setSelectedDeviceIds([]);
    };
    const onApply = () => {
        if (!applyingTemplate || selectedDeviceIds.length === 0) return;
        applyMutation.mutate(
            { template_id: applyingTemplate.id, device_ids: selectedDeviceIds },
            {
                onSuccess: () => {
                    setApplyingTemplate(null);
                    queryClient.invalidateQueries({ queryKey: alertKeys.all });
                },
            }
        );
    };

    // ---- 表格列定义 ----
    const ruleColumns: ColumnsType<Alert.RuleItem> = [
        { title: '规则名称', dataIndex: 'name', ellipsis: true },
        { title: '关联设备', dataIndex: 'device_name', ellipsis: true },
        {
            title: '严重级别',
            dataIndex: 'severity',
            width: 100,
            render: (v: Alert.Severity) => (
                <Tag color={SEVERITY_COLORS[v]}>{SEVERITY_LABELS[v] || v}</Tag>
            ),
        },
        {
            title: '条件数',
            dataIndex: 'conditions',
            width: 80,
            render: (conditions: Alert.Condition[]) => conditions?.length ?? 0,
        },
        {
            title: '逻辑',
            dataIndex: 'logic',
            width: 80,
            render: (v: string) => <Tag>{v === 'and' ? '全部满足' : '任一满足'}</Tag>,
        },
        {
            title: '状态',
            dataIndex: 'status',
            width: 80,
            render: (v: 'enabled' | 'disabled') => <StatusTag status={v} />,
        },
        {
            title: '操作',
            key: 'actions',
            width: 150,
            fixed: 'right' as const,
            render: (_, record) => (
                <Space>
                    {canEdit && (
                        <Button type="link" onClick={() => openRuleEdit(record)}>
                            编辑
                        </Button>
                    )}
                    {canDelete && (
                        <Button type="link" danger onClick={() => onRuleDelete(record)}>
                            删除
                        </Button>
                    )}
                </Space>
            ),
        },
    ];

    const tplColumns: ColumnsType<Alert.TemplateItem> = [
        { title: '模板名称', dataIndex: 'name', ellipsis: true },
        {
            title: '设备类型',
            dataIndex: 'config_name',
            width: 140,
            render: (v: string, record) =>
                v ? (
                    <span>
                        {v}
                        <Tag className="ml-1" bordered={false}>
                            {record.protocol_type}
                        </Tag>
                    </span>
                ) : (
                    '-'
                ),
        },
        { title: '分类', dataIndex: 'category', width: 100 },
        {
            title: '严重级别',
            dataIndex: 'severity',
            width: 100,
            render: (v: Alert.Severity) => (
                <Tag color={SEVERITY_COLORS[v]}>{SEVERITY_LABELS[v] || v}</Tag>
            ),
        },
        {
            title: '操作',
            key: 'actions',
            width: 200,
            fixed: 'right' as const,
            render: (_, record) => (
                <Space>
                    {canAdd && (
                        <Button type="link" onClick={() => openApplyModal(record)}>
                            应用
                        </Button>
                    )}
                    {canEdit && (
                        <Button type="link" onClick={() => openTplEdit(record)}>
                            编辑
                        </Button>
                    )}
                    {canDelete && (
                        <Button type="link" danger onClick={() => onTplDelete(record)}>
                            删除
                        </Button>
                    )}
                </Space>
            ),
        },
    ];

    return (
        <Modal
            open={open}
            title="规则配置"
            onCancel={onClose}
            footer={null}
            width={1000}
            destroyOnClose
        >
            {/* ---- 规则模板 ---- */}
            <div className="flex items-center justify-between flex-wrap gap-2 mb-2">
                <span className="font-medium text-base">规则模板</span>
                {canAdd && (
                    <Button type="primary" onClick={openTplCreate}>
                        新建模板
                    </Button>
                )}
            </div>
            <Table<Alert.TemplateItem>
                rowKey="id"
                columns={tplColumns}
                dataSource={templatePage?.list || []}
                loading={tplLoading}
                pagination={{
                    current: tplPagination.page,
                    pageSize: tplPagination.pageSize,
                    total: templatePage?.total || 0,
                    showSizeChanger: true,
                    showTotal: (total, range) => `${range[0]}-${range[1]} / 共 ${total} 条`,
                }}
                onChange={(p) =>
                    setTplPagination({ page: p.current || 1, pageSize: p.pageSize || 5 })
                }
                size="small"
                scroll={{ x: 'max-content' }}
            />

            <Divider className="my-3" />

            {/* ---- 告警规则 ---- */}
            <div className="flex items-center justify-between flex-wrap gap-2 mb-2">
                <Space wrap>
                    <span className="font-medium text-base">告警规则</span>
                    <Search
                        allowClear
                        placeholder="规则名称 / 备注"
                        onChange={(e) => debouncedRuleSearch(e.target.value)}
                        onSearch={doRuleSearch}
                        className="w-[200px]"
                    />
                    <Select
                        value={ruleSeverity}
                        onChange={(v) => {
                            setRuleSeverity(v);
                            setRulePagination((prev) => ({ ...prev, page: 1 }));
                        }}
                        options={SEVERITY_OPTIONS}
                        className="w-[120px]"
                    />
                </Space>
                <Space>
                    {canDelete && ruleSelectedKeys.length > 0 && (
                        <Button
                            danger
                            onClick={onRuleBatchDelete}
                            loading={ruleBatchDeleteMutation.isPending}
                        >
                            批量删除 ({ruleSelectedKeys.length})
                        </Button>
                    )}
                    {canAdd && (
                        <Button type="primary" onClick={openRuleCreate}>
                            新建规则
                        </Button>
                    )}
                </Space>
            </div>
            <Table<Alert.RuleItem>
                rowKey="id"
                columns={ruleColumns}
                dataSource={rulePage?.list || []}
                loading={ruleLoading}
                rowSelection={
                    canDelete
                        ? {
                              selectedRowKeys: ruleSelectedKeys,
                              onChange: (keys) => setRuleSelectedKeys(keys as string[]),
                          }
                        : undefined
                }
                pagination={{
                    current: rulePagination.page,
                    pageSize: rulePagination.pageSize,
                    total: rulePage?.total || 0,
                    showSizeChanger: true,
                    showTotal: (total, range) => `${range[0]}-${range[1]} / 共 ${total} 条`,
                }}
                onChange={(p) =>
                    setRulePagination({ page: p.current || 1, pageSize: p.pageSize || 5 })
                }
                size="small"
                scroll={{ x: 'max-content' }}
            />

            {/* 子弹窗 */}
            <AlertRuleFormModal
                open={ruleFormVisible}
                editing={ruleEditing}
                saveMutation={ruleSaveMutation}
                onClose={() => {
                    setRuleFormVisible(false);
                    setRuleEditing(null);
                }}
            />
            <AlertTemplateFormModal
                open={tplFormVisible}
                editing={tplEditing}
                editingDetail={tplEditingDetail}
                saveMutation={tplSaveMutation}
                onClose={() => {
                    setTplFormVisible(false);
                    setTplEditing(null);
                    setTplEditingDetail(null);
                }}
            />
            <ApplyTemplateModal
                open={!!applyingTemplate}
                templateName={applyingTemplate?.name ?? ''}
                selectedDeviceIds={selectedDeviceIds}
                onDeviceIdsChange={setSelectedDeviceIds}
                onOk={onApply}
                onCancel={() => setApplyingTemplate(null)}
                loading={applyMutation.isPending}
            />
        </Modal>
    );
}

// ==================== 应用模板弹窗 ====================

function ApplyTemplateModal({
    open,
    templateName,
    selectedDeviceIds,
    onDeviceIdsChange,
    onOk,
    onCancel,
    loading,
}: {
    open: boolean;
    templateName: string;
    selectedDeviceIds: string[];
    onDeviceIdsChange: (ids: string[]) => void;
    onOk: () => void;
    onCancel: () => void;
    loading: boolean;
}) {
    const { data: deviceOptionsData } = useDeviceOptions({ enabled: open });
    const deviceOptions = deviceOptionsData ?? [];

    return (
        <Modal
            open={open}
            title={`应用模板「${templateName}」`}
            onCancel={onCancel}
            onOk={onOk}
            confirmLoading={loading}
            okButtonProps={{ disabled: selectedDeviceIds.length === 0 }}
        >
            <div className="mb-2">选择目标设备：</div>
            <Select
                mode="multiple"
                value={selectedDeviceIds}
                onChange={onDeviceIdsChange}
                options={deviceOptions.map((d) => ({ label: d.name, value: d.id }))}
                showSearch
                optionFilterProp="label"
                placeholder="搜索并选择设备"
                className="w-full"
                maxTagCount="responsive"
            />
            {selectedDeviceIds.length > 0 && (
                <div className="mt-2 text-gray-500">已选择 {selectedDeviceIds.length} 个设备</div>
            )}
        </Modal>
    );
}

// ==================== 主页面 ====================

const AlertPage = () => {
    const canQuery = usePermission('iot:alert:query');
    const canAck = usePermission('iot:alert:ack');

    const [severity, setSeverity] = useState('');
    const [status, setStatus] = useState('');
    const [pagination, setPagination] = useState({ page: 1, pageSize: 20 });
    const [selectedRowKeys, setSelectedRowKeys] = useState<string[]>([]);
    const [configModalOpen, setConfigModalOpen] = useState(false);
    const { modal } = App.useApp();

    const { data: stats, isLoading: statsLoading } = useAlertStats({ enabled: canQuery });

    const { data: recordPage, isLoading } = useAlertRecordList(
        {
            page: pagination.page,
            pageSize: pagination.pageSize,
            severity: severity || undefined,
            status: status || undefined,
        },
        { enabled: canQuery }
    );

    const ackMutation = useAlertAcknowledge();
    const batchAckMutation = useAlertBatchAcknowledge();

    if (!canQuery) {
        return (
            <PageContainer>
                <Result status="403" title="无权限" subTitle="您没有查询告警的权限，请联系管理员" />
            </PageContainer>
        );
    }

    const onAcknowledge = (record: Alert.RecordItem) => {
        modal.confirm({
            title: '确认告警',
            content: `确认告警「${record.message}」？`,
            okText: '确认',
            onOk: () => ackMutation.mutate(record.id),
        });
    };

    const onBatchAcknowledge = () => {
        if (selectedRowKeys.length === 0) return;
        modal.confirm({
            title: '批量确认',
            content: `确认选中的 ${selectedRowKeys.length} 条告警？`,
            okText: '确认',
            onOk: () =>
                batchAckMutation.mutate(selectedRowKeys, {
                    onSuccess: () => setSelectedRowKeys([]),
                }),
        });
    };

    const handleTableChange = (paginationConfig: TablePaginationConfig) => {
        setPagination({
            page: paginationConfig.current || 1,
            pageSize: paginationConfig.pageSize || 20,
        });
    };

    const recordColumns: ColumnsType<Alert.RecordItem> = [
        { title: '设备', dataIndex: 'device_name', ellipsis: true, width: 120 },
        {
            title: '级别',
            dataIndex: 'severity',
            width: 80,
            render: (v: Alert.Severity) => (
                <Tag color={SEVERITY_COLORS[v]}>{SEVERITY_LABELS[v] || v}</Tag>
            ),
        },
        { title: '告警消息', dataIndex: 'message', ellipsis: true },
        {
            title: '触发时间',
            dataIndex: 'triggered_at',
            width: DATE_TIME_COLUMN_WIDTH,
            render: (value: string) => formatDateTime(value),
        },
        {
            title: '状态',
            dataIndex: 'status',
            width: 90,
            render: (v: Alert.RecordStatus) => (
                <Tag color={STATUS_COLORS[v]}>{STATUS_LABELS[v] || v}</Tag>
            ),
        },
        {
            title: '确认时间',
            dataIndex: 'acknowledged_at',
            width: DATE_TIME_COLUMN_WIDTH,
            render: (value?: string) => formatDateTime(value),
        },
        {
            title: '操作',
            key: 'actions',
            width: 80,
            fixed: 'right' as const,
            render: (_, record) =>
                canAck && record.status === 'active' ? (
                    <Button type="link" onClick={() => onAcknowledge(record)}>
                        确认
                    </Button>
                ) : null,
        },
    ];

    return (
        <PageContainer>
            {/* 概要统计 + 管理入口 */}
            <div className="flex items-center justify-between flex-wrap gap-2 mb-4">
                {statsLoading ? (
                    <Skeleton.Input active size="small" style={{ width: 500 }} />
                ) : (
                    <Space size="middle" wrap>
                        <span className="font-medium">
                            活跃告警
                            <span className="text-lg font-semibold ml-1">{stats?.total ?? 0}</span>
                        </span>
                        <Tag color="red">严重 {stats?.critical ?? 0}</Tag>
                        <Tag color="orange">警告 {stats?.warning ?? 0}</Tag>
                        <Tag color="blue">信息 {stats?.info ?? 0}</Tag>
                        <Tag color="cyan">已确认 {stats?.acknowledged ?? 0}</Tag>
                        <Tag color="green">今日恢复 {stats?.today_resolved ?? 0}</Tag>
                        <Tag>今日新增 {stats?.today_new ?? 0}</Tag>
                        <Tag>涉及设备 {stats?.affected_devices ?? 0}</Tag>
                    </Space>
                )}
                <Button onClick={() => setConfigModalOpen(true)}>规则配置</Button>
            </div>

            {/* 记录筛选 + 操作 */}
            <div className="flex items-center justify-between flex-wrap gap-2 mb-4">
                <Space wrap>
                    <Select
                        value={severity}
                        onChange={(v) => {
                            setSeverity(v);
                            setPagination((prev) => ({ ...prev, page: 1 }));
                        }}
                        options={SEVERITY_OPTIONS}
                        className="w-[120px]"
                    />
                    <Select
                        value={status}
                        onChange={(v) => {
                            setStatus(v);
                            setPagination((prev) => ({ ...prev, page: 1 }));
                        }}
                        options={STATUS_OPTIONS}
                        className="w-[120px]"
                    />
                </Space>
                <Space>
                    {canAck && selectedRowKeys.length > 0 && (
                        <Button
                            type="primary"
                            onClick={onBatchAcknowledge}
                            loading={batchAckMutation.isPending}
                        >
                            批量确认 ({selectedRowKeys.length})
                        </Button>
                    )}
                </Space>
            </div>

            {/* 告警记录表 */}
            <Table<Alert.RecordItem>
                rowKey="id"
                columns={recordColumns}
                dataSource={recordPage?.list || []}
                loading={isLoading}
                rowSelection={
                    canAck
                        ? {
                              selectedRowKeys,
                              onChange: (keys) => setSelectedRowKeys(keys as string[]),
                              getCheckboxProps: (record) => ({
                                  disabled: record.status !== 'active',
                              }),
                          }
                        : undefined
                }
                pagination={{
                    current: pagination.page,
                    pageSize: pagination.pageSize,
                    total: recordPage?.total || 0,
                    showSizeChanger: true,
                    showTotal: (total, range) => `${range[0]}-${range[1]} / 共 ${total} 条`,
                }}
                onChange={handleTableChange}
                size="middle"
                scroll={{ x: 'max-content' }}
                sticky
            />

            {/* 规则配置弹窗 */}
            <RuleConfigModal open={configModalOpen} onClose={() => setConfigModalOpen(false)} />
        </PageContainer>
    );
};

export default AlertPage;
