import { parseDateTime } from '@/utils/dateTime';
import type { Device } from './device.types';

export const isDeviceOnline = (device: Device.RealTimeData, now = Date.now()) => {
    if (!device.reportTime) return false;
    const reportTime = parseDateTime(device.reportTime);
    if (!reportTime || Number.isNaN(reportTime.getTime())) return false;
    return now - reportTime.getTime() <= (device.online_timeout || 300) * 1000;
};
