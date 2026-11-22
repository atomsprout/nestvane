#pragma once

// The host's battery, for the guest to show as its own. The host's side writes it whenever it
// changes; the kernel layer reads it when the guest's health service reads the power supplies.

#include <atomic>

namespace nestvane {

struct GuestBattery {
    enum State { unknown, unplugged, charging, full };
    // Percent, or -1 when the host does not say: then the guest has no battery to read at all.
    std::atomic<int> level{-1};
    std::atomic<int> state{unknown};
    static GuestBattery& host() {
        static GuestBattery battery;
        return battery;
    }
};

} // namespace nestvane
