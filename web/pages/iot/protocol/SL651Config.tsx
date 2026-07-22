/**
 * SL651 协议配置页面
 * 参考原项目 iot-frontend/src/views/SL651/TypeConfig
 * 布局：左侧设备类型列表 + 右侧功能码/要素配置
 */

import { DownloadOutlined, UploadOutlined } from '@ant-design/icons';
import {
    Button,
    Card,
    Empty,
    Flex,
    Popconfirm,
    Result,
    Skeleton,
    Space,
    Tag,
    Tooltip,
    Tree,
} from 'antd';
import { type CSSProperties, type ReactNode, useCallback, useMemo, useRef, useState } from 'react';
import { PageContainer } from '@/components/PageContainer';
import { usePermissions } from '@/hooks/usePermission';
import {
    buildGroupSections,
    getGroupKey,
    reorderItemsByGroupOrder,
    reorderItemsWithinGroupOrder,
} from './grouping';
import {
    useProtocolConfigDelete,
    useProtocolConfigList,
    useProtocolConfigSave,
} from './protocol.service';
import type { SL651 } from './protocol.types';
import {
    SortableGroupItemList,
    SortableGroupSectionFrame,
    SortableGroupSectionList,
    VirtualStack,
} from './SortableGroup';
import {
    DeviceTypeModal,
    type DeviceTypeModalRef,
    DictConfigModal,
    type DictConfigModalRef,
    ElementModal,
    type ElementModalRef,
    FuncModal,
    type FuncModalRef,
    PresetValueModal,
    type PresetValueModalRef,
    ResponseElementsModal,
    type ResponseElementsModalRef,
} from './sl651';
import { useProtocolImportExport } from './useProtocolImportExport';

/** 带要素的功能码 */
interface FuncWithElements extends SL651.Func {
    elements: SL651.Element[];
    responseElements?: SL651.Element[];
}

const ELEMENT_CARD_GRID_STYLE: CSSProperties = {
    gridTemplateColumns: 'repeat(auto-fill, minmax(280px, 1fr))',
};

const SL651ConfigPage = () => {
    // 权限检查
    const { has } = usePermissions();
    const canQuery = has('iot:protocol:query');
    const canAdd = has('iot:protocol:add');
    const canEdit = has('iot:protocol:edit');
    const canDelete = has('iot:protocol:delete');
    const canImport = has('iot:protocol:import');
    const canExport = has('iot:protocol:export');

    // 设备类型列表查询
    const {
        data: configPage,
        isLoading: loadingTypes,
        refetch: refetchTypes,
    } = useProtocolConfigList({ protocol: 'SL651' }, { enabled: canQuery });

    // 保存和删除 mutations
    const saveMutation = useProtocolConfigSave();
    const deleteMutation = useProtocolConfigDelete();

    // 导入导出
    const { exportConfigs, triggerImport, importing } = useProtocolImportExport('SL651');

    // 当前选中的设备类型 ID（用户手动选择）
    const [selectedTypeId, setSelectedTypeId] = useState<string>();

    // Modal refs
    const deviceTypeModalRef = useRef<DeviceTypeModalRef>(null);
    const funcModalRef = useRef<FuncModalRef>(null);
    const elementModalRef = useRef<ElementModalRef>(null);
    const presetValueModalRef = useRef<PresetValueModalRef>(null);
    const dictConfigModalRef = useRef<DictConfigModalRef>(null);
    const responseElementsModalRef = useRef<ResponseElementsModalRef>(null);

    // 设备类型列表（使用 useMemo 保持引用稳定）
    const types = useMemo(() => configPage?.list || [], [configPage?.list]);
    const emptyTypeDesc = types.length ? '未选择设备类型' : '暂无设备类型';

    // 计算当前激活的类型 ID：优先用户选择，否则默认第一个
    const activeTypeId = useMemo(() => {
        if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
            return selectedTypeId;
        }
        return types.length > 0 ? types[0].id : undefined;
    }, [selectedTypeId, types]);

    // 功能码列表（派生状态，根据 activeTypeId 计算）
    const funcs = useMemo<FuncWithElements[]>(() => {
        if (!activeTypeId) return [];
        const type = types.find((t) => t.id === activeTypeId);
        if (!type) return [];
        const config = type.config as SL651.Config;
        return config?.funcs || [];
    }, [activeTypeId, types]);

    // 加载状态（与数据加载状态同步）
    const loadingFuncs = loadingTypes;

    // ========== 设备类型操作 ==========

    const handleDeleteDeviceType = async () => {
        if (!activeTypeId) return;
        // 删除前记录下一个可选中的类型
        const idx = types.findIndex((t) => t.id === activeTypeId);
        const nextType = types[idx + 1] ?? types[idx - 1];
        await deleteMutation.mutateAsync(activeTypeId);
        setSelectedTypeId(nextType?.id);
    };

    // ========== 功能码操作 ==========

    const handleDeleteFunc = async (funcId: string) => {
        if (!activeTypeId) return;
        const type = types.find((t) => t.id === activeTypeId);
        if (!type) return;

        const config = type.config as SL651.Config;
        const newConfig: SL651.Config = {
            funcs: config.funcs.filter((f) => f.id !== funcId),
        };

        await saveMutation.mutateAsync({
            id: activeTypeId,
            protocol: 'SL651',
            config: newConfig,
        });
    };

    // ========== 要素操作 ==========

    const handleDeleteElement = async (funcId: string, eleId: string) => {
        if (!activeTypeId) return;
        const type = types.find((t) => t.id === activeTypeId);
        if (!type) return;

        const config = type.config as SL651.Config;
        const newConfig: SL651.Config = {
            funcs: config.funcs.map((f) =>
                f.id === funcId ? { ...f, elements: f.elements.filter((e) => e.id !== eleId) } : f
            ),
        };

        await saveMutation.mutateAsync({
            id: activeTypeId,
            protocol: 'SL651',
            config: newConfig,
        });
    };

    const persistElements = useCallback(
        async (
            funcId: string,
            listKind: 'elements' | 'responseElements',
            nextItems: SL651.Element[],
            currentItems: SL651.Element[]
        ) => {
            if (!activeTypeId) return;
            const type = types.find((t) => t.id === activeTypeId);
            if (!type) return;

            if (nextItems.length === currentItems.length) {
                const isSameOrder =
                    nextItems.every((item, index) => item.id === currentItems[index]?.id) &&
                    nextItems.every(
                        (item, index) =>
                            getGroupKey(item.group) === getGroupKey(currentItems[index]?.group)
                    );
                if (isSameOrder) return;
            }

            const config = type.config as SL651.Config;
            const newFuncs = config.funcs.map((func) => {
                if (func.id !== funcId) return func;
                if (listKind === 'responseElements') {
                    return {
                        ...func,
                        responseElements: nextItems.length > 0 ? nextItems : undefined,
                    };
                }
                return {
                    ...func,
                    elements: nextItems,
                };
            });

            await saveMutation.mutateAsync({
                id: activeTypeId,
                protocol: 'SL651',
                config: { funcs: newFuncs },
            });
            await refetchTypes();
        },
        [activeTypeId, refetchTypes, saveMutation, types]
    );

    const handleReorderElements = useCallback(
        async (
            funcId: string,
            listKind: 'elements' | 'responseElements',
            nextOrder: string[],
            currentItems: SL651.Element[]
        ) => {
            await persistElements(
                funcId,
                listKind,
                reorderItemsByGroupOrder(currentItems, nextOrder),
                currentItems
            );
        },
        [persistElements]
    );

    const handleReorderElementItems = useCallback(
        async (
            funcId: string,
            listKind: 'elements' | 'responseElements',
            groupKey: string,
            nextOrder: string[],
            currentItems: SL651.Element[]
        ) => {
            await persistElements(
                funcId,
                listKind,
                reorderItemsWithinGroupOrder(currentItems, groupKey, nextOrder),
                currentItems
            );
        },
        [persistElements]
    );

    const renderElementCard = (
        element: SL651.Element,
        funcId: string,
        funcDir: SL651.Direction,
        options?: { advancedActions?: boolean },
        dragHandle?: ReactNode
    ) => {
        const advancedActions = options?.advancedActions !== false;
        const presetValueCount = element.options?.length ?? 0;
        const dictItemCount = element.dictConfig?.items?.length ?? 0;
        const showPresetValueTag = funcDir === 'DOWN' && advancedActions && presetValueCount > 0;
        const showDictTag = advancedActions && element.encode === 'DICT' && dictItemCount > 0;

        return (
            <Card
                key={element.id}
                size="small"
                hoverable
                className="h-full border-slate-200 shadow-[0_1px_4px_rgba(15,23,42,0.06)]"
                styles={{ body: { padding: 12 } }}
            >
                <Flex justify="space-between" gap={12} align="start" className="mb-2">
                    <div className="min-w-0 flex-1">
                        <div className="truncate text-sm font-semibold text-slate-800">
                            {element.name}
                        </div>
                        <div className="mt-0.5 text-[12px] text-slate-400">
                            引导符 {element.guideHex}
                        </div>
                    </div>
                    <Space size={4} className="shrink-0">
                        {dragHandle}
                        {canEdit && (
                            <Button
                                size="small"
                                type="link"
                                onClick={() => {
                                    if (activeTypeId)
                                        elementModalRef.current?.open(
                                            'edit',
                                            activeTypeId,
                                            funcId,
                                            element
                                        );
                                }}
                            >
                                编辑
                            </Button>
                        )}
                        {canDelete && (
                            <Popconfirm
                                title="确认删除？"
                                onConfirm={() => handleDeleteElement(funcId, element.id)}
                            >
                                <Button size="small" danger type="link">
                                    删除
                                </Button>
                            </Popconfirm>
                        )}
                    </Space>
                </Flex>

                <Space size={6} wrap className="mb-2">
                    <Tag color="blue">{element.encode}</Tag>
                    <Tag>{element.length} 字节</Tag>
                    {typeof element.digits === 'number' ? (
                        <Tag>小数位数 {element.digits}</Tag>
                    ) : null}
                    {element.unit ? <Tag>{element.unit}</Tag> : null}
                    {showPresetValueTag ? (
                        <Tag color="cyan">{presetValueCount} 个预设值</Tag>
                    ) : null}
                    {showDictTag ? <Tag color="purple">{dictItemCount} 个字典项</Tag> : null}
                </Space>

                {element.remark ? (
                    <div className="text-xs leading-5 text-slate-500">{element.remark}</div>
                ) : (
                    <div className="text-xs leading-5 text-slate-400">暂无备注</div>
                )}

                {(funcDir === 'DOWN' && advancedActions && canEdit) ||
                (advancedActions && element.encode === 'DICT' && canEdit) ? (
                    <Space size={4} wrap className="mt-3">
                        {funcDir === 'DOWN' && advancedActions && canEdit && (
                            <Button
                                size="small"
                                onClick={() => {
                                    if (activeTypeId)
                                        presetValueModalRef.current?.open(
                                            activeTypeId,
                                            funcId,
                                            element
                                        );
                                }}
                            >
                                预设值
                            </Button>
                        )}
                        {advancedActions && element.encode === 'DICT' && canEdit && (
                            <Button
                                size="small"
                                onClick={() => {
                                    if (activeTypeId)
                                        dictConfigModalRef.current?.open(
                                            activeTypeId,
                                            funcId,
                                            element
                                        );
                                }}
                            >
                                字典
                            </Button>
                        )}
                    </Space>
                ) : null}
            </Card>
        );
    };

    const renderGroupedElements = (
        elements: SL651.Element[] | undefined,
        funcId: string,
        funcDir: SL651.Direction,
        emptyDescription: string,
        options?: { advancedActions?: boolean; listKind?: 'elements' | 'responseElements' }
    ) => {
        const advancedActions = options?.advancedActions !== false;
        const listKind = options?.listKind ?? 'elements';
        const groups = buildGroupSections(elements || []);
        if (!groups.length) {
            return <Empty description={emptyDescription} />;
        }

        return (
            <SortableGroupSectionList
                sections={groups}
                className="w-full"
                disabled={saveMutation.isPending || !canEdit}
                onOrderChange={(nextOrder) =>
                    handleReorderElements(funcId, listKind, nextOrder, elements || [])
                }
                empty={<Empty description={emptyDescription} />}
            >
                {(group) => (
                    <SortableGroupSectionFrame
                        id={group.key}
                        key={group.key}
                        className="rounded-xl border border-slate-200 bg-white p-3"
                        bodyClassName="mt-4"
                        disabled={saveMutation.isPending || !canEdit}
                        title={group.label}
                        meta={<Tag color="blue">{group.count} 个</Tag>}
                    >
                        <SortableGroupItemList
                            items={group.items}
                            className="grid gap-3"
                            style={ELEMENT_CARD_GRID_STYLE}
                            minItemWidth={280}
                            disabled={saveMutation.isPending || !canEdit}
                            empty={<Empty description={emptyDescription} />}
                            onOrderChange={(nextOrder) =>
                                handleReorderElementItems(
                                    funcId,
                                    listKind,
                                    group.key,
                                    nextOrder,
                                    elements || []
                                )
                            }
                        >
                            {(element, dragHandle) =>
                                renderElementCard(
                                    element,
                                    funcId,
                                    funcDir,
                                    { advancedActions },
                                    dragHandle
                                )
                            }
                        </SortableGroupItemList>
                    </SortableGroupSectionFrame>
                )}
            </SortableGroupSectionList>
        );
    };

    const renderFuncCard = (record: FuncWithElements) => {
        const elementCount = record.elements?.length ?? 0;
        const responseCount = record.responseElements?.length ?? 0;

        return (
            <Card
                key={record.id}
                size="small"
                hoverable
                className="w-full border-slate-200 shadow-[0_1px_4px_rgba(15,23,42,0.06)]"
                styles={{ body: { padding: 16 } }}
            >
                <Flex justify="space-between" gap={12} align="start" className="mb-2">
                    <div className="min-w-0 flex-1">
                        <div className="truncate text-sm font-semibold text-slate-800">
                            {record.name}
                        </div>
                        <div className="mt-0.5 text-[12px] text-slate-400">
                            功能码 {record.funcCode}
                        </div>
                    </div>
                    <Space size={4} className="shrink-0">
                        {canEdit && (
                            <Button
                                size="small"
                                type="link"
                                onClick={() => {
                                    if (activeTypeId)
                                        funcModalRef.current?.open('edit', activeTypeId, record);
                                }}
                            >
                                编辑
                            </Button>
                        )}
                        {canDelete && (
                            <Popconfirm
                                title="确认删除？"
                                onConfirm={() => handleDeleteFunc(record.id)}
                            >
                                <Button size="small" danger type="link">
                                    删除
                                </Button>
                            </Popconfirm>
                        )}
                    </Space>
                </Flex>

                <Space size={6} wrap className="mb-2">
                    <Tag color={record.dir === 'UP' ? 'green' : 'orange'}>
                        {record.dir === 'UP' ? '上行' : '下行'}
                    </Tag>
                    <Tag color="blue">{elementCount} 个要素</Tag>
                    {record.dir === 'DOWN' ? (
                        <Tag color="cyan">{responseCount} 个应答要素</Tag>
                    ) : null}
                </Space>

                {record.remark ? (
                    <div className="text-xs leading-5 text-slate-500">{record.remark}</div>
                ) : (
                    <div className="text-xs leading-5 text-slate-400">暂无备注</div>
                )}

                <Space size={4} wrap className="mt-3">
                    {record.dir === 'DOWN' && canEdit && (
                        <Button
                            size="small"
                            onClick={() => {
                                if (activeTypeId)
                                    responseElementsModalRef.current?.open(activeTypeId, record);
                            }}
                        >
                            应答要素
                        </Button>
                    )}
                </Space>

                <div className="mt-4 space-y-4">
                    <div>
                        <Flex justify="space-between" align="center" gap={12} className="mb-2">
                            <div className="text-sm font-semibold text-slate-700">要素配置</div>
                            {canAdd && (
                                <Button
                                    size="small"
                                    type="primary"
                                    onClick={() => {
                                        if (activeTypeId)
                                            elementModalRef.current?.open(
                                                'create',
                                                activeTypeId,
                                                record.id
                                            );
                                    }}
                                >
                                    新增要素
                                </Button>
                            )}
                        </Flex>
                        {renderGroupedElements(record.elements, record.id, record.dir, '暂无要素', {
                            listKind: 'elements',
                        })}
                    </div>
                    {record.dir === 'DOWN' && (
                        <div>
                            <div className="mb-2 text-sm font-semibold text-slate-700">
                                应答要素
                            </div>
                            {renderGroupedElements(
                                record.responseElements,
                                record.id,
                                record.dir,
                                '暂无应答要素',
                                {
                                    advancedActions: false,
                                    listKind: 'responseElements',
                                }
                            )}
                        </div>
                    )}
                </div>
            </Card>
        );
    };

    const emptyFuncDesc = activeTypeId ? '暂无功能码，点击右上角新增功能码' : emptyTypeDesc;

    // 权限检查
    if (!canQuery) {
        return (
            <PageContainer title="SL651配置">
                <Result status="403" title="无权访问" subTitle="您没有权限访问此页面" />
            </PageContainer>
        );
    }

    return (
        <PageContainer title="SL651配置">
            <div className="flex h-full min-h-0 overflow-hidden">
                {/* 左侧：设备类型列表 */}
                <div className="h-full min-h-0 w-[360px] shrink-0 pr-3">
                    <Card
                        title="设备类型"
                        className="flex h-full min-h-0 flex-col overflow-hidden"
                        styles={{
                            body: { flex: 1, minHeight: 0, overflow: 'auto', padding: 16 },
                        }}
                        extra={
                            <Space size={4}>
                                {canAdd && (
                                    <Button
                                        size="small"
                                        type="primary"
                                        onClick={() => deviceTypeModalRef.current?.open('create')}
                                    >
                                        新增
                                    </Button>
                                )}
                                {canEdit && (
                                    <Button
                                        size="small"
                                        disabled={!activeTypeId}
                                        onClick={() =>
                                            deviceTypeModalRef.current?.open(
                                                'edit',
                                                types.find((t) => t.id === activeTypeId)
                                            )
                                        }
                                    >
                                        编辑
                                    </Button>
                                )}
                                {canDelete && (
                                    <Popconfirm
                                        title="确认删除该设备类型？"
                                        onConfirm={handleDeleteDeviceType}
                                        disabled={!activeTypeId}
                                    >
                                        <Button size="small" danger disabled={!activeTypeId}>
                                            删除
                                        </Button>
                                    </Popconfirm>
                                )}
                                {canExport && (
                                    <Tooltip title="导出">
                                        <Button
                                            size="small"
                                            icon={<DownloadOutlined />}
                                            disabled={!types.length}
                                            onClick={() => exportConfigs(types)}
                                        />
                                    </Tooltip>
                                )}
                                {canImport && (
                                    <Tooltip title="导入">
                                        <Button
                                            size="small"
                                            icon={<UploadOutlined />}
                                            loading={importing}
                                            onClick={triggerImport}
                                        />
                                    </Tooltip>
                                )}
                            </Space>
                        }
                    >
                        {loadingTypes ? (
                            <Skeleton active paragraph={{ rows: 6 }} />
                        ) : types.length === 0 ? (
                            <Empty description="暂无设备类型" />
                        ) : (
                            <Tree
                                blockNode
                                className="[&_.ant-tree-switcher]:hidden"
                                selectedKeys={activeTypeId ? [String(activeTypeId)] : []}
                                onSelect={(keys) => {
                                    if (keys.length > 0) {
                                        setSelectedTypeId(String(keys[0]));
                                    }
                                }}
                                treeData={types.map((t) => {
                                    const config = t.config as SL651.Config;
                                    const mode = config?.responseMode || 'M1';
                                    return {
                                        key: String(t.id),
                                        title: (
                                            <Tooltip
                                                title={t.remark || '暂无备注'}
                                                placement="right"
                                            >
                                                <Flex
                                                    justify="space-between"
                                                    align="center"
                                                    className="h-8 p-1"
                                                >
                                                    <Space size={4}>
                                                        <span>{t.name}</span>
                                                        <Tag color="blue">{mode}</Tag>
                                                    </Space>
                                                    {t.enabled ? (
                                                        <Tag color="green">启用</Tag>
                                                    ) : (
                                                        <Tag color="red">禁用</Tag>
                                                    )}
                                                </Flex>
                                            </Tooltip>
                                        ),
                                    };
                                })}
                            />
                        )}
                    </Card>
                </div>

                {/* 右侧：功能码配置 */}
                <div className="h-full min-h-0 min-w-0 flex-1">
                    <Card
                        title={
                            activeTypeId
                                ? '功能码配置'
                                : types.length > 0
                                  ? '请选择设备类型'
                                  : '暂无设备类型'
                        }
                        className="flex h-full min-h-0 flex-col overflow-hidden"
                        styles={{
                            body: { flex: 1, minHeight: 0, overflow: 'auto', padding: 0 },
                        }}
                        extra={
                            activeTypeId &&
                            canAdd && (
                                <Button
                                    type="primary"
                                    onClick={() =>
                                        funcModalRef.current?.open('create', activeTypeId)
                                    }
                                >
                                    新增功能码
                                </Button>
                            )
                        }
                    >
                        {!activeTypeId ? (
                            <Empty description={emptyTypeDesc} />
                        ) : loadingFuncs ? (
                            <Skeleton active paragraph={{ rows: 6 }} className="p-4" />
                        ) : funcs.length === 0 ? (
                            <Empty description={emptyFuncDesc} />
                        ) : (
                            <VirtualStack
                                items={funcs}
                                getItemKey={(record) => record.id}
                                className="p-4"
                                estimateSize={520}
                                gap={16}
                            >
                                {(record) => renderFuncCard(record)}
                            </VirtualStack>
                        )}
                    </Card>
                </div>
            </div>

            {/* 设备类型 Modal */}
            <DeviceTypeModal
                ref={deviceTypeModalRef}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />

            {/* 功能码 Modal */}
            <FuncModal
                ref={funcModalRef}
                types={types}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />

            {/* 要素 Modal */}
            <ElementModal
                ref={elementModalRef}
                types={types}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />

            {/* 预设值 Modal */}
            <PresetValueModal
                ref={presetValueModalRef}
                types={types}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />

            {/* 字典配置 Modal */}
            <DictConfigModal
                ref={dictConfigModalRef}
                types={types}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />

            {/* 应答要素 Modal */}
            <ResponseElementsModal
                ref={responseElementsModalRef}
                types={types}
                onSuccess={refetchTypes}
                saveMutation={saveMutation}
            />
        </PageContainer>
    );
};

export default SL651ConfigPage;
