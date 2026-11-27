#pragma once

#include "GuestVideo.hpp"

#include <string>

namespace nestvane {
// The phone's own H.264 and HEVC decoders (VideoToolbox), for Android's codec service.
GuestVideoDecoder::Factory makeIOSVideoDecoders();
// What the decoders did since the last call, in words, for the rate report (NESTVANE_DISPLAY_RATE).
std::string IOSVideoDecoderReport();
}
