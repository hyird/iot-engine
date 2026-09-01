import { expect, test } from 'bun:test';
import { renderToStaticMarkup } from 'react-dom/server';
import DeviceCard from '../web/components/DeviceCard';

test('grouped device cards preserve configured element names', () => {
    const markup = renderToStaticMarkup(
        <DeviceCard
            title="测试设备"
            items={[
                {
                    key: 'gate-position',
                    label: '1#闸当前闸位值',
                    children: '0.00 mm',
                    group: '1#状态',
                },
            ]}
        />
    );

    expect(markup).toContain('>1#闸当前闸位值<');
    expect(markup).not.toContain('>当前闸位值<');
});

test('clickable device cards expose an accessible detail entry', () => {
    const markup = renderToStaticMarkup(
        <DeviceCard
            title="边缘节点"
            ariaLabel="查看边缘节点 860406088541915"
            onClick={() => undefined}
            items={[{ key: 'state', label: '状态', children: '在线' }]}
            extra={<span>查看详情</span>}
        />
    );

    expect(markup).toContain('<button');
    expect(markup).toContain('type="button"');
    expect(markup).toContain('aria-label="查看边缘节点 860406088541915"');
    expect(markup).toContain('>查看详情<');
});
