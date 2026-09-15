#pragma once

#include <cstdint>
#include <string>

namespace service::collector::dlt645 {

enum class Version { V1997, V2007 };

struct Connection {
    Version version = Version::V2007;
    std::uint8_t wakeupBytes = 4;
    std::string writePassword;
    std::string operatorCode;
    bool operator==(const Connection&) const = default;
};

} // namespace service::collector::dlt645
