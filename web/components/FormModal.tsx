import type { CSSProperties } from 'react';
import { Modal } from 'antd';
import type { ModalProps } from 'antd';

const modalContainerStyle: CSSProperties = {
    maxHeight: '90vh',
    display: 'flex',
    flexDirection: 'column',
};

export function FormModal(props: ModalProps) {
    const { styles, centered = true, ...rest } = props;
    const customStyles = typeof styles === 'function' ? undefined : styles;

    return (
        <Modal
            centered={centered}
            styles={
                {
                    ...customStyles,
                    container: {
                        ...modalContainerStyle,
                        ...customStyles?.container,
                    },
                    body: {
                        flex: 1,
                        minHeight: 0,
                        overflowY: 'auto',
                        ...customStyles?.body,
                    },
                } as ModalProps['styles']
            }
            {...rest}
        />
    );
}
