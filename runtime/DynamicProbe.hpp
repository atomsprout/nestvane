#pragma once

#include <string>

namespace nestvane {

struct DynamicReport {
    bool passed;
    std::string report;
};

DynamicReport probeArt(const std::string& directory);
DynamicReport probeDynamic(const std::string& directory);
DynamicReport probeInit(const std::string& directory, const std::string& databasePath = {});

} // namespace nestvane
