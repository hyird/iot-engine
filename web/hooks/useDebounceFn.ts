import { useCallback, useEffect, useRef } from 'react';

/**
 * 函数防抖 Hook，替代 ahooks 的 useDebounceFn
 */
export function useDebounceFn<Args extends unknown[], R>(
    fn: (...args: Args) => R,
    wait = 300
): { run: (...args: Args) => void; cancel: () => void } {
    const timerRef = useRef<ReturnType<typeof setTimeout>>(null);
    const fnRef = useRef(fn);
    fnRef.current = fn;

    const cancel = useCallback(() => {
        if (timerRef.current) {
            clearTimeout(timerRef.current);
            timerRef.current = null;
        }
    }, []);

    const run = useCallback(
        (...args: Args) => {
            cancel();
            timerRef.current = setTimeout(() => fnRef.current(...args), wait);
        },
        [cancel, wait]
    );

    useEffect(() => cancel, [cancel]);

    return { run, cancel };
}
