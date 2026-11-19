#pragma once

#include <string>

namespace nestvane {
struct Validation {
    bool passed;
    std::string report;
};

Validation validate(const std::string& directory);
} // namespace nestvane
