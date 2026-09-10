#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/features/gb28181/device/device.types.h"

struct DeviceRouteSnapshot {
    bool online{ false };
    std::string remoteAddress;
    bool hasChannels{ false };
    bool channelExists{ false };
};

class DeviceRegistry {
  public:
    using Observer = std::function<void(const Device&, DeviceChange)>;

    explicit DeviceRegistry(Observer observer = {})
        : observer_(std::move(observer)) {}

    void upsertRegistration(const std::string& deviceId, const std::string& remoteAddress, const std::string& source = "sip");
    void updateKeepalive(const std::string& deviceId, const std::string& remoteAddress);
    bool updateKeepaliveAndNeedsCatalog(const std::string& deviceId, const std::string& remoteAddress);
    void updateCatalog(const std::string& deviceId, std::vector<Channel> channels);
    void updateRecords(const std::string& deviceId, std::vector<RecordItem> records);
    void forEachDevice(const std::function<void(const Device&)>& visitor) const;
    bool visitDevice(const std::string& deviceId, const std::function<void(const Device&)>& visitor) const;
    std::vector<Device> listDevices() const;
    std::optional<Device> findDevice(const std::string& deviceId) const;
    std::optional<DeviceRouteSnapshot>
    findRouteSnapshot(const std::string& deviceId, const std::string& channelId = {}) const;
    void markOffline(const std::string& deviceId);
    bool updateMapping(const std::string& deviceId, std::string mappedDeviceId);
    bool updateDeviceName(const std::string& deviceId, std::string customName);
    bool updateChannelName(const std::string& deviceId, const std::string& channelId, std::string customName);
    void replace(std::vector<Device> devices);

  private:
    void notify(const Device& device, DeviceChange change) const;

    std::unordered_map<std::string, Device> devices_;
    Observer observer_;
    std::uint64_t nextSessionGeneration_{ 1 };
};
