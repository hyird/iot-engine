#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

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
        std::cout << "GB28181 projector tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "GB28181 projector test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
