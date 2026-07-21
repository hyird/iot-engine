import type { CSSProperties } from 'react';
import { Modal } from 'antd';
import type { ModalProps } from 'antd';

const modalContainerStyle: CSSProperties = {
    height: 'min(720px, 90dvh)',
    maxHeight: '90dvh',
    display: 'flex',
    flexDirection: 'column',
    overflow: 'hidden',
};

const modalBodyStyle: CSSProperties = {
    flex: '1 1 auto',
    minHeight: 0,
    overflowX: 'hidden',
    overflowY: 'auto',
};

const fixedSectionStyle: CSSProperties = {
    flex: '0 0 auto',
};

const FORM_MODAL_WIDTH = 720;

export function FormModal(props: ModalProps) {
    const { styles, centered = true, ...rest } = props;
    const customStyles = typeof styles === 'function' ? undefined : styles;

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
                        ...fixedSectionStyle,
                    },
                    body: {
                        ...customStyles?.body,
                        ...modalBodyStyle,
                    },
                    footer: {
                        ...customStyles?.footer,
                        ...fixedSectionStyle,
                    },
                } as ModalProps['styles']
            }
        />
    );
}
