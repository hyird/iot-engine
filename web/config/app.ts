const fallbackAppName = '云边端数据采集管控一体化平台';
export const APP_NAME = import.meta.env.VITE_APP_NAME?.trim() || fallbackAppName;
export const SUPERADMIN_ROLE_CODE = 'superadmin';

export function getAppTitle(suffix?: string) {
    return suffix ? `${APP_NAME} - ${suffix}` : APP_NAME;
}
