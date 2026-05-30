#pragma once

#include <stdexcept>

namespace DeviceTests {

inline void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace DeviceTests
