import type { ModalProps } from 'antd';
import { Modal, theme } from 'antd';
import type { CSSProperties } from 'react';

const modalContainerStyle: CSSProperties = {
    height: 'min(720px, 90dvh)',
    maxHeight: '90dvh',
    display: 'flex',
    flexDirection: 'column',
    overflow: 'hidden',
    padding: 0,
};

const FORM_MODAL_WIDTH = 720;

export function FormModal(props: ModalProps) {
    const { styles, centered = true, ...rest } = props;
    const customStyles = typeof styles === 'function' ? undefined : styles;
    const { token } = theme.useToken();
    const modalBodyStyle: CSSProperties = {
        flex: '1 1 auto',
        minHeight: 0,
        overflowX: 'hidden',
        overflowY: 'auto',
        padding: '20px 24px',
        background: token.colorBgContainer,
    };
    const modalHeaderStyle: CSSProperties = {
        flex: '0 0 auto',
        margin: 0,
        padding: '18px 24px 16px',
        borderBottom: `1px solid ${token.colorSplit}`,
    };
    const modalFooterStyle: CSSProperties = {
        flex: '0 0 auto',
        margin: 0,
        padding: '14px 24px 16px',
        borderTop: `1px solid ${token.colorSplit}`,
    };

    return (
        <Modal
            {...rest}
            centered={centered}
            width={FORM_MODAL_WIDTH}
            styles={
                {
                    ...customStyles,
                    container: {
                        ...customStyles?.container,
                        ...modalContainerStyle,
                    },
                    header: {
                        ...customStyles?.header,
                        ...modalHeaderStyle,
                    },
                    body: {
                        ...customStyles?.body,
                        ...modalBodyStyle,
                    },
                    footer: {
                        ...customStyles?.footer,
                        ...modalFooterStyle,
                    },
                } as ModalProps['styles']
            }
        />
    );
}
