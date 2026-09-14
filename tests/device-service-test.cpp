#include <array>
#include <iostream>
#include <stdexcept>
#include "service/modules/device/device.service.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void verifyDeviceCapabilities() {
    using namespace service::device;
    constexpr std::array levels{DeviceAccessLevel::none, DeviceAccessLevel::view,
        DeviceAccessLevel::operate, DeviceAccessLevel::owner};
    for (const auto level : levels) {
        for (unsigned permissions = 0; permissions != 16; ++permissions) {
            DeviceActor actor;
            actor.canEdit = (permissions & 1) != 0;
            actor.canDelete = (permissions & 2) != 0;
            actor.canShare = (permissions & 4) != 0;
            actor.canCommand = (permissions & 8) != 0;
            for (const bool remoteControl : {false, true}) {
                const auto result = DeviceAccessService::capabilities(actor, level, remoteControl);
                const bool owner = level == DeviceAccessLevel::owner;
                const bool operatorAccess = owner || level == DeviceAccessLevel::operate;
                require(result.canEdit == (owner && actor.canEdit), "edit requires ownership and permission");
                require(result.canDelete == (owner && actor.canDelete), "delete requires ownership and permission");
                require(result.canShare == (owner && actor.canShare), "share requires ownership and permission");
                require(result.canCommand == (operatorAccess && remoteControl && actor.canCommand),
                        "command requires operate access, remote control and permission");
            }
        }
    }
    require(DeviceAccessService::rank("0") == DeviceAccessLevel::none, "none rank");
    require(DeviceAccessService::rank("1") == DeviceAccessLevel::view, "view rank");
    require(DeviceAccessService::rank("2") == DeviceAccessLevel::operate, "operate rank");
    require(DeviceAccessService::rank("4") == DeviceAccessLevel::owner, "owner rank");
    for (const auto invalid : {"", "-1", "1junk", "nan", "9999999999999999999999999"})
        require(DeviceAccessService::rank(invalid) == DeviceAccessLevel::none, "invalid rank must not grant access");
}
}
int main() {
    try {
        verifyDeviceCapabilities();
        std::cout << "device access behavior tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "device service test failed: " << error.what() << '\n';
        return 1;
    }
}
