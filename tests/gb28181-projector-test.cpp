#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "service/features/gb28181/device/DeviceRegistry.h"
#include "service/features/gb28181/projector.h"

namespace {

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

} // namespace

int main() {
  try {
    require(service::gb28181::Projector::integerForTest("12", -1) == 12,
            "GB28181 projector integer parser changed valid integer");
    require(service::gb28181::Projector::integerForTest("12x", -1) == -1,
            "GB28181 projector integer parser accepted trailing garbage");

    Device device;
    device.id = "camera-1";
    device.name = "Reported camera";
    device.customName = "Production camera";
    device.channels.push_back(Channel{
        .id = "channel-1",
        .name = "Reported channel",
        .customName = "Gate camera",
    });
    DeviceRegistry registry;
    registry.replace({device});
    registry.updateCatalog(
        device.id,
        {Channel{.id = "channel-1", .name = "Changed reported channel"}});
    const auto refreshed = registry.findDevice(device.id);
    require(refreshed.has_value(), "renamed GB28181 device disappeared");
    require(refreshed->displayName() == "Production camera",
            "device custom name was not preserved");
    require(refreshed->channels.size() == 1 &&
                refreshed->channels[0].displayName() == "Gate camera",
            "catalog refresh replaced the channel custom name");
    require(registry.updateDeviceName(device.id, "Renamed camera"),
            "device rename failed");
    require(
        registry.updateChannelName(device.id, "channel-1", "Renamed channel"),
        "channel rename failed");
    const auto renamed = registry.findDevice(device.id);
    require(renamed && renamed->displayName() == "Renamed camera" &&
                renamed->channels[0].displayName() == "Renamed channel",
            "custom names were not updated in the runtime registry");
    std::cout << "GB28181 projector tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "GB28181 projector test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
