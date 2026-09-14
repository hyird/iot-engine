export interface SessionUser {
    id: string;
    username: string;
    nickname?: string;
    status: string;
    roles: { id: string; name: string; code: string }[];
    permissions: string[];
}
