#include "device/DeviceRegistry.h"

#include <algorithm>
#include <chrono>
#include <utility>

void DeviceRegistry::upsertRegistration(const std::string &deviceId,
                                        const std::string &remoteAddress,
                                        const std::string &source) {
  auto &device = devices_[deviceId];
  device.id = deviceId;
  if (device.name.empty()) {
    device.name = deviceId;
  }
  device.remoteAddress = remoteAddress;
  device.registrationSource = source;
  device.online = true;
  device.lastSeen = std::chrono::system_clock::now();
  notify(device, Change::Status);
}

void DeviceRegistry::updateKeepalive(const std::string &deviceId,
                                     const std::string &remoteAddress) {
  auto &device = devices_[deviceId];
  device.id = deviceId;
  if (device.name.empty()) {
    device.name = deviceId;
  }
  device.remoteAddress = remoteAddress;
  device.registrationSource = "sip";
  device.online = true;
  device.lastSeen = std::chrono::system_clock::now();
  notify(device, Change::Status);
}

bool DeviceRegistry::updateKeepaliveAndNeedsCatalog(
    const std::string &deviceId, const std::string &remoteAddress) {
  const auto iter = devices_.find(deviceId);
  const auto needsCatalog =
      iter == devices_.end() || iter->second.channels.empty();

  auto &device = devices_[deviceId];
  device.id = deviceId;
  if (device.name.empty()) {
    device.name = deviceId;
  }
  device.remoteAddress = remoteAddress;
  device.registrationSource = "sip";
  device.online = true;
  device.lastSeen = std::chrono::system_clock::now();
  notify(device, Change::Status);
  return needsCatalog;
}

void DeviceRegistry::updateCatalog(const std::string &deviceId,
                                   std::vector<Channel> channels) {
  auto &device = devices_[deviceId];
  device.id = deviceId;
  if (device.name.empty()) {
    device.name = deviceId;
  }
  device.online = true;
  device.lastSeen = std::chrono::system_clock::now();
  for (auto &channel : channels) {
    const auto current = std::find_if(
        device.channels.begin(), device.channels.end(),
        [&](const Channel &value) { return value.id == channel.id; });
    if (current != device.channels.end())
      channel.customName = current->customName;
  }
  device.channels = std::move(channels);
  notify(device, Change::Catalog);
}

void DeviceRegistry::updateRecords(const std::string &deviceId,
                                   std::vector<RecordItem> records) {
  auto &device = devices_[deviceId];
  device.id = deviceId;
  if (device.name.empty()) {
    device.name = deviceId;
  }
  device.online = true;
  device.lastSeen = std::chrono::system_clock::now();
  device.records = std::move(records);
  notify(device, Change::Records);
}

void DeviceRegistry::forEachDevice(
    const std::function<void(const Device &)> &visitor) const {
  for (const auto &[_, device] : devices_) {
    visitor(device);
  }
}

bool DeviceRegistry::visitDevice(
    const std::string &deviceId,
    const std::function<void(const Device &)> &visitor) const {
  const auto iter = devices_.find(deviceId);
  if (iter == devices_.end()) {
    return false;
  }
  visitor(iter->second);
  return true;
}

std::vector<Device> DeviceRegistry::listDevices() const {
  std::vector<Device> result;
  result.reserve(devices_.size());
  for (const auto &[_, device] : devices_) {
    result.push_back(device);
  }
  return result;
}

std::optional<Device>
DeviceRegistry::findDevice(const std::string &deviceId) const {
  const auto iter = devices_.find(deviceId);
  if (iter == devices_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

std::optional<DeviceRouteSnapshot>
DeviceRegistry::findRouteSnapshot(const std::string &deviceId,
                                  const std::string &channelId) const {
  const auto iter = devices_.find(deviceId);
  if (iter == devices_.end()) {
    return std::nullopt;
  }

  const auto &device = iter->second;
  DeviceRouteSnapshot snapshot;
  snapshot.online = device.online;
  snapshot.remoteAddress = device.remoteAddress;
  snapshot.hasChannels = !device.channels.empty();
  if (!channelId.empty()) {
    snapshot.channelExists = std::any_of(
        device.channels.begin(), device.channels.end(),
        [&](const Channel &channel) { return channel.id == channelId; });
  }
  return snapshot;
}

void DeviceRegistry::markOffline(const std::string &deviceId) {
  const auto iter = devices_.find(deviceId);
  if (iter != devices_.end()) {
    iter->second.online = false;
    notify(iter->second, Change::Status);
  }
}

bool DeviceRegistry::updateMapping(const std::string &deviceId,
                                   std::string mappedDeviceId) {
  const auto iter = devices_.find(deviceId);
  if (iter == devices_.end())
    return false;
  iter->second.mappedDeviceId = std::move(mappedDeviceId);
  notify(iter->second, Change::Mapping);
  return true;
}

bool DeviceRegistry::updateDeviceName(const std::string &deviceId,
                                      std::string customName) {
  const auto iter = devices_.find(deviceId);
  if (iter == devices_.end())
    return false;
  iter->second.customName = std::move(customName);
  notify(iter->second, Change::DeviceName);
  return true;
}

bool DeviceRegistry::updateChannelName(const std::string &deviceId,
                                       const std::string &channelId,
                                       std::string customName) {
  const auto device = devices_.find(deviceId);
  if (device == devices_.end())
    return false;
  const auto channel = std::find_if(
      device->second.channels.begin(), device->second.channels.end(),
      [&](const Channel &value) { return value.id == channelId; });
  if (channel == device->second.channels.end())
    return false;
  channel->customName = std::move(customName);
  notify(device->second, Change::ChannelName);
  return true;
}

void DeviceRegistry::replace(std::vector<Device> devices) {
  devices_.clear();
  for (auto &device : devices)
    devices_.emplace(device.id, std::move(device));
}

void DeviceRegistry::notify(const Device &device, Change change) const {
  if (observer_)
    observer_(device, change);
}
