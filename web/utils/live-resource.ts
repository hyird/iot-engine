export interface LiveObserver<T> {
    next: (value: T) => void;
    error: (error: Error) => void;
}

/** A lazy subscription. Awaiting it reads one SSE snapshot, never a JSON GET. */
export class LiveResource<T> implements PromiseLike<T> {
    constructor(readonly subscribe: (observer: LiveObserver<T>) => () => void) {}

    static value<T>(value: T): LiveResource<T> {
        return new LiveResource((observer) => { observer.next(value); return () => {}; });
    }

    static combine<T extends readonly unknown[]>(sources: { [K in keyof T]: LiveResource<T[K]> }): LiveResource<T> {
        return new LiveResource((observer) => {
            const values: unknown[] = new Array(sources.length);
            const ready = new Set<number>();
            const releases = sources.map((source, index) => source.subscribe({
                next: (value) => {
                    values[index] = value;
                    ready.add(index);
                    if (ready.size === sources.length) observer.next([...values] as unknown as T);
                }, error: observer.error,
            }));
            return () => { for (const release of releases) release(); };
        });
    }

    switchMap<U>(transform: (value: T) => LiveResource<U>): LiveResource<U> {
        return new LiveResource((observer) => {
            let releaseChild: (() => void) | undefined;
            const release = this.subscribe({
                next: (value) => {
                    releaseChild?.();
                    releaseChild = undefined;
                    try { releaseChild = transform(value).subscribe(observer); }
                    catch (error) { observer.error(error instanceof Error ? error : new Error(String(error))); }
                }, error: observer.error,
            });
            return () => { release(); releaseChild?.(); };
        });
    }

    map<U>(transform: (value: T) => U): LiveResource<U> {
        return new LiveResource((observer) => this.subscribe({
            next: (value) => {
                try { observer.next(transform(value)); }
                catch (error) { observer.error(error instanceof Error ? error : new Error(String(error))); }
            },
            error: observer.error,
        }));
    }

    filter(predicate: (value: T) => boolean): LiveResource<T> {
        return new LiveResource((observer) => this.subscribe({
            next: (value) => {
                try { if (predicate(value)) observer.next(value); }
                catch (error) { observer.error(error instanceof Error ? error : new Error(String(error))); }
            }, error: observer.error,
        }));
    }

    first(signal?: AbortSignal): Promise<T> {
        return new Promise((resolve, reject) => {
            let release: (() => void) | undefined;
            let finished = false;
            const cleanup = () => {
                finished = true;
                release?.();
                signal?.removeEventListener('abort', abort);
            };
            const abort = () => { cleanup(); reject(signal?.reason ?? new DOMException('Aborted', 'AbortError')); };
            if (signal?.aborted) { abort(); return; }
            signal?.addEventListener('abort', abort, { once: true });
            release = this.subscribe({
                next: (value) => { cleanup(); resolve(value); },
                error: (error) => { cleanup(); reject(error); },
            });
            if (finished) release();
        });
    }

    then<TResult1 = T, TResult2 = never>(
        fulfilled?: ((value: T) => TResult1 | PromiseLike<TResult1>) | null,
        rejected?: ((reason: unknown) => TResult2 | PromiseLike<TResult2>) | null,
    ): Promise<TResult1 | TResult2> {
        return this.first().then(fulfilled, rejected);
    }
}
