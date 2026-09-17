#pragma once
#include "../vendor/json.hpp"
#include <string_view>
namespace iotvpn {
using Json = nlohmann::json;
inline constexpr std::size_t MaxMessageBytes = 1024 * 1024;
Json parseJson(std::string_view text);
}
