#pragma once

#include <stdexcept>

namespace service::auth {

class JwtExpiredError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class JwtInvalidError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace service::auth
