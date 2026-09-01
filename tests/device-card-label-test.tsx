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
