export type DateTimeValue = Date | number | string | null | undefined;

const pad = (value: number) => String(value).padStart(2, '0');

export function parseDateTime(value: Exclude<DateTimeValue, null | undefined>) {
    if (value instanceof Date) return new Date(value.getTime());
    if (typeof value === 'number') return new Date(value);

    const trimmed = value.trim();
    if (!trimmed) return null;

    if (/^\d+$/.test(trimmed)) {
        return new Date(Number(trimmed));
    }

    const normalized = trimmed
        .replace(/^(\d{4}-\d{2}-\d{2})\s+/, '$1T')
        .replace(/([+-]\d{2})(\d{2})$/, '$1:$2')
        .replace(/([+-]\d{2})$/, '$1:00');
    return new Date(normalized);
}

export function formatDateTime(value: DateTimeValue) {
    if (value === null || value === undefined || value === '') return '--';

    const date = parseDateTime(value);
    if (!date || Number.isNaN(date.getTime())) return '--';

    return [
        `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`,
        `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`,
    ].join(' ');
}
