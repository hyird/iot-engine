import type { FormInstance } from 'antd';
import type { ZodType } from 'zod';

export function validateForm<T>(form: FormInstance, schema: ZodType<T>, values: unknown): T | null {
    const result = schema.safeParse(values);
    if (result.success) {
        return result.data;
    }

    const errors = new Map<string, string[]>();
    for (const issue of result.error.issues) {
        const name = String(issue.path[0] ?? '');
        if (!name) continue;
        errors.set(name, [...(errors.get(name) ?? []), issue.message]);
    }

    const valueNames =
        typeof values === 'object' && values !== null
            ? Object.keys(values as Record<string, unknown>)
            : [];
    const names = new Set([...valueNames, ...errors.keys()]);
    form.setFields(
        [...names].map((name) => ({
            name,
            errors: errors.get(name) ?? [],
        }))
    );

    return null;
}
