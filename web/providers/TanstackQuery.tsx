import { QueryClientProvider } from '@tanstack/react-query';
import type { ReactNode } from 'react';
import { queryClient } from '@/utils/query';

export interface TanstackQueryProps {
    children: ReactNode;
}

export function TanstackQuery({ children }: TanstackQueryProps) {
    return <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>;
}
