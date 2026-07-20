/// <reference types="vite/client" />

interface ImportMetaEnv {
    readonly VITE_APP_NAME?: string;
}

declare module '@ant-design/icons/es/icons/*' {
    import type { ComponentType, CSSProperties } from 'react';
    const Icon: ComponentType<{ style?: CSSProperties; className?: string }>;
    export default Icon;
}
