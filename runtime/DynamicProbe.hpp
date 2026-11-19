#pragma once

#include "GraphicsPipe.hpp"
#include "GuestCamera.hpp"
#include "GuestTouch.hpp"

#include <memory>

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
DynamicReport probeInit(const std::string& directory, const std::string& databasePath = {}, const BootProgress& progress = {},
                        GraphicsPipeFactory graphics = {}, std::shared_ptr<GuestTouch> touch = {},
                        std::shared_ptr<GuestCameraSource> camera = {});

} // namespace nestvane
