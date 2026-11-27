#pragma once

#include "GraphicsPipe.hpp"
#include "GuestApkInbox.hpp"
#include "GuestCamera.hpp"
#include "GuestTouch.hpp"
#include "GuestColorDma.hpp"

#include <memory>
#include <stop_token>

#include <functional>
#include <string>
#include <string_view>

namespace nestvane {

// The files of the system image that are mapped just now: path, where, how long. For reading in
// ahead of time what a start uses (ios/CodePrefetch.hpp). The visitor runs with the list held still.
struct MappedImageFile {
    std::string path;
    const uint8_t* data;
    size_t size;
    // Which file Android opened first is 1, and so on; 0 for a file it has not opened.
    uint32_t order = 0;
};
void visitMappedImageFiles(const std::function<void(const MappedImageFile&)>& visit);

struct DynamicReport {
    bool passed;
    std::string report;
};

// Export Diagnostics: asks the Android that runs for a report of where it stands, for when it seems
// frozen. The report has what each guest thread waits for, memory, the stacks of the system server's
// Java threads and the last of Android's log; it comes to `done` some seconds later, on the thread
// that runs Android. False when no Android runs. A request while one is pending replaces it.
bool requestDiagnostics(std::function<void(std::string)> done);

DynamicReport probeArt(const std::string& directory);
DynamicReport probeDynamic(const std::string& directory);
using BootProgress = std::function<void(std::string_view)>;
DynamicReport probeInit(const std::string& directory, const std::string& databasePath = {}, const BootProgress& progress = {},
                        GraphicsPipeFactory graphics = {}, std::shared_ptr<GuestTouch> touch = {},
                        std::shared_ptr<GuestCameraSource> camera = {},
                        std::shared_ptr<GuestApkInbox> apkInbox = {}, std::stop_token stop = {},
                        const BootProgress& watchdog = {}, const BootProgress& liveSnapshot = {},
                        std::shared_ptr<GuestColorDma> graphicsDma = {});

} // namespace nestvane
