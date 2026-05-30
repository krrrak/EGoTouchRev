#pragma once

#include <stdexcept>
#include <string>

namespace Solvers::Tests {

inline void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace Solvers::Tests
