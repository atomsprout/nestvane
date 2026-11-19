#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace nestvane {

struct DynamicReport {
    bool passed;
    std::string report;
};

DynamicReport probeArt(const std::string& directory);
DynamicReport probeDynamic(const std::string& directory);
using BootProgress = std::function<void(std::string_view)>;
DynamicReport probeInit(const std::string& directory, const std::string& databasePath = {}, const BootProgress& progress = {});

} // namespace nestvane
