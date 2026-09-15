export function retainNewerRecords(previous: unknown, incoming: unknown): unknown {
    if (!Array.isArray(previous) || !Array.isArray(incoming)) return incoming;
    const known = new Map(previous.map((record) => [record.id, record]));
    return incoming.map((record) => {
        const existing = known.get(record.id);
        return existing && Number(existing.revision) > Number(record.revision) ? existing : record;
    });
}
