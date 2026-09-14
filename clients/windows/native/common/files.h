#pragma once
#include "json.h"
#include <filesystem>
#include <span>
#include <vector>
namespace iotvpn {
std::filesystem::path stateDirectory();
std::filesystem::path moduleDirectory();
std::vector<std::uint8_t> readFile(const std::filesystem::path& path, std::size_t limit = MaxMessageBytes);
void atomicWrite(const std::filesystem::path& path, std::span<const std::uint8_t> data);

}
