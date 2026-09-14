#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
namespace iotvpn {
std::vector<std::uint8_t> protectData(std::span<const std::uint8_t> data, std::string_view entropy = {}, std::wstring_view description = {});
std::vector<std::uint8_t> unprotectData(std::span<const std::uint8_t> data, std::string_view entropy = {});

}
