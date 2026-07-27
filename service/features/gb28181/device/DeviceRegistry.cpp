#include "device/DeviceRegistry.h"

#include <algorithm>
#include <chrono>
#include <utility>

void DeviceRegistry::upsertRegistration(const std::string& deviceId, const std::string& remoteAddress, const std::string& source) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.remoteAddress = remoteAddress;
    device.registrationSource = source;
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
}

void DeviceRegistry::updateKeepalive(const std::string& deviceId, const std::string& remoteAddress) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.remoteAddress = remoteAddress;
    device.registrationSource = "sip";
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
}

bool DeviceRegistry::updateKeepaliveAndNeedsCatalog(const std::string& deviceId, const std::string& remoteAddress) {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    const auto needsCatalog = iter == devices_.end() || iter->second.channels.empty();

    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.remoteAddress = remoteAddress;
    device.registrationSource = "sip";
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
    return needsCatalog;
}

void DeviceRegistry::updateCatalog(const std::string& deviceId, std::vector<Channel> channels) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
    device.channels = std::move(channels);
}

void DeviceRegistry::updateRecords(const std::string& deviceId, std::vector<RecordItem> records) {
    std::lock_guard lock(mutex_);
    auto& device = devices_[deviceId];
    device.id = deviceId;
    if (device.name.empty()) {
        device.name = deviceId;
    }
    device.online = true;
    device.lastSeen = std::chrono::system_clock::now();
    device.records = std::move(records);
}

void DeviceRegistry::forEachDevice(const std::function<void(const Device&)>& visitor) const {
    std::lock_guard lock(mutex_);
    for (const auto& [_, device] : devices_) {
        visitor(device);
    }
}

bool DeviceRegistry::visitDevice(const std::string& deviceId, const std::function<void(const Device&)>& visitor) const {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    if (iter == devices_.end()) {
        return false;
    }
    visitor(iter->second);
    return true;
}

std::vector<Device> DeviceRegistry::listDevices() const {
    std::lock_guard lock(mutex_);
    std::vector<Device> result;
    result.reserve(devices_.size());
    for (const auto& [_, device] : devices_) {
        result.push_back(device);
    }
    return result;
}

std::optional<Device> DeviceRegistry::findDevice(const std::string& deviceId) const {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    if (iter == devices_.end()) {
        return std::nullopt;
    }
    return iter->second;
}

std::optional<DeviceRouteSnapshot> DeviceRegistry::findRouteSnapshot(const std::string& deviceId, const std::string& channelId) const {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    if (iter == devices_.end()) {
        return std::nullopt;
    }

    const auto& device = iter->second;
    DeviceRouteSnapshot snapshot;
    snapshot.online = device.online;
    snapshot.remoteAddress = device.remoteAddress;
    snapshot.hasChannels = !device.channels.empty();
    if (!channelId.empty()) {
        snapshot.channelExists = std::any_of(device.channels.begin(), device.channels.end(), [&](const Channel& channel) {
            return channel.id == channelId;
        });
    }
    return snapshot;
}

void DeviceRegistry::markOffline(const std::string& deviceId) {
    std::lock_guard lock(mutex_);
    const auto iter = devices_.find(deviceId);
    if (iter != devices_.end()) {
        iter->second.online = false;
    }
}
