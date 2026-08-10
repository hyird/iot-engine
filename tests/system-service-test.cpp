#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string readSource(const char* relative) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / relative;
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open system service source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeStoll(const char* relative, const char* message) {
    const auto source = readSource(relative);
    require(source.find("std::stoll(") == std::string::npos, message);
}

void requireRolePermissionValidation() {
    const auto source = readSource("service/domains/role/role.service.h");
    require(source.find("权限编码不能为空") != std::string::npos,
            "role service accepts empty permission codes");
    require(source.find("value.size() > 128") != std::string::npos,
            "role service accepts overlong permission codes");
}

void requireUserRoleIdsDeduplicated() {
    const auto source = readSource("service/domains/user/user.service.h");
    require(source.find("角色不能重复") != std::string::npos,
            "user service accepts duplicate role_ids");
    require(source.find("seenRoleIds") != std::string::npos,
            "user service does not track duplicate role_ids before replacing roles");
}

} // namespace

int main() {
    try {
        requireNoUnsafeStoll("service/domains/user/user.service.h",
                             "user service uses unsafe/partial stoll parsing");
        requireNoUnsafeStoll("service/domains/role/role.service.h",
                             "role service uses unsafe/partial stoll parsing");
        requireNoUnsafeStoll("service/domains/dept/dept.service.h",
                             "department service uses unsafe/partial stoll parsing");
        requireRolePermissionValidation();
        requireUserRoleIdsDeduplicated();
        std::cout << "system service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "system service test failed: " << error.what() << '\n';
        return 1;
    }
}
