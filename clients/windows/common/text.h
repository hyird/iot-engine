#pragma once
#include <string>
#include <string_view>
namespace iotvpn {
std::wstring utf16(std::string_view text);
std::string utf8(std::wstring_view text);
}
