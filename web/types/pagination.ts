export interface PageParams {
    page?: number | string;
    pageSize?: number | string;
    keyword?: string;
}

export interface PaginatedResult<T> {
    list: T[];
    total: number;
    page?: number;
    pageSize?: number;
    totalPages?: number;
}
