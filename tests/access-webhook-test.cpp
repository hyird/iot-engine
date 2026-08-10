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

std::string webhookSource() {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                "service/features/access/webhook.h";
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open access webhook source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireNoUnsafeStoll(std::string_view source) {
    require(source.find("std::stoll(") == std::string_view::npos,
            "access webhook uses unsafe/partial stoll parsing");
}

} // namespace

int main() {
    try {
        requireNoUnsafeStoll(webhookSource());
        std::cout << "access webhook tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "access webhook test failed: " << error.what() << '\n';
        return 1;
    }
}
