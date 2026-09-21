const IDLE_MS = 5 * 60 * 1000;
const timers = new Map<string, ReturnType<typeof setTimeout>>();

export function armDebugIdleOff(key: string, turnOff: () => void) {
    const previous = timers.get(key);
    if (previous) clearTimeout(previous);
    timers.set(
        key,
        setTimeout(() => {
            timers.delete(key);
            turnOff();
        }, IDLE_MS)
    );
}

export function disarmDebugIdleOff(key: string) {
    const previous = timers.get(key);
    if (!previous) return;
    clearTimeout(previous);
    timers.delete(key);
}
