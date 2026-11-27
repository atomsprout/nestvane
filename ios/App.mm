#import <CoreGraphics/CoreGraphics.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Network/Network.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#include "Backup.hpp"
#include "GuestAudio.hpp"
#include "GuestBattery.hpp"
#include "DynamicProbe.hpp"
#include "ProbeLinux.hpp"
#include "GfxstreamPipe.hpp"
#include "CodePrefetch.hpp"
#include "GfxstreamSession.hpp"
#include "HostSocket.hpp"
#include "IOSCamera.hpp"
#include "IOSVideoDecoder.hpp"
#include "Validation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <mach/mach.h>
#include <map>
extern "C" int proc_regionfilename(int pid, uint64_t address, void* buffer, uint32_t size);
#include <memory>
#include <new>
#include <thread>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <vector>

// Where the output of Android's sound stands, in words, for the rate report (NESTVANE_DISPLAY_RATE).
static NSString* GuestAudioOutputState = @"not started";
static NSString* GuestMicrophoneState = @"idle";

static UIImage* ImageOfSurface(IOSurfaceRef surface) {
    if (!surface || IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != 0) return nil;
    const size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
    const size_t stride = IOSurfaceGetBytesPerRow(surface);
    NSData* pixels = [NSData dataWithBytes:IOSurfaceGetBaseAddress(surface) length:stride * height];
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGBitmapInfo bitmap = kCGBitmapByteOrder32Little | CGBitmapInfo(kCGImageAlphaNoneSkipFirst);
    CGImageRef frame = provider && color
        ? CGImageCreate(width, height, 8, 32, stride, color, bitmap, provider, nullptr, false, kCGRenderingIntentDefault)
        : nullptr;
    UIImage* image = frame ? [UIImage imageWithCGImage:frame] : nil;
    if (frame) CGImageRelease(frame);
    if (color) CGColorSpaceRelease(color);
    if (provider) CGDataProviderRelease(provider);
    return image;
}

// Whether a surface holds more than one colour: the same question guestDisplayHasColorVariation asks of pixels read back.
static bool SurfaceHasColorVariation(IOSurfaceRef surface) {
    if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != 0) return false;
    const size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface);
    const size_t stride = IOSurfaceGetBytesPerRow(surface);
    const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
    bool varies = false;
    if (base && width && height) {
        uint32_t first;
        std::memcpy(&first, base, 4);
        for (size_t y = 0; y < height && !varies; ++y) {
            const uint8_t* row = base + y * stride;
            for (size_t x = 0; x < width; ++x) {
                uint32_t pixel;
                std::memcpy(&pixel, row + x * 4, 4);
                if ((pixel ^ first) & 0x00ffffff) { varies = true; break; }
            }
        }
    }
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    return varies;
}

// While the phone is hot Android draws less. At "serious" the most frames a second is half the rate
// the display runs at, at "critical" 30; the screen is asked for frames no more often than that, and
// Android is told to draw no more (GuestApkInbox::submitDisplayRate). The display itself keeps its
// rate, which Android reads once as it starts.
// NESTVANE_TEST_HOT="<state>:<seconds>" pretends the phone reached that state so long after launch.
static std::atomic_int HeatPretended{-1};
static NSProcessInfoThermalState HeatNow() {
    const int pretended = HeatPretended.load(std::memory_order_relaxed);
    return pretended >= 0 ? NSProcessInfoThermalState(pretended) : NSProcessInfo.processInfo.thermalState;
}
static int RateInHeat(int rate) {
    switch (HeatNow()) {
        case NSProcessInfoThermalStateCritical: return std::min(rate, 30);
        case NSProcessInfoThermalStateSerious: return std::max(rate / 2, 30);
        default: return rate;
    }
}

// The screen's refresh, for the thread that takes Android's frames: it waits here and is woken each
// time the screen is about to show a new frame, at most |rate| times a second.
@interface ScreenRefresh : NSObject
- (instancetype)initWithRate:(int)rate;
- (void)wait;
- (unsigned)takeTicks;
- (void)stop;
@end

@implementation ScreenRefresh {
    CADisplayLink* _link;
    dispatch_semaphore_t _tick;
    std::atomic_uint _ticks;
    int _rate;
}

- (instancetype)initWithRate:(int)rate {
    if ((self = [super init])) {
        _tick = dispatch_semaphore_create(0);
        _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(refreshed:)];
        _rate = rate;
        [self heatChanged];
        [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(heatChanged)
            name:NSProcessInfoThermalStateDidChangeNotification object:nil];
    }
    return self;
}

- (void)heatChanged {
    dispatch_async(dispatch_get_main_queue(), ^{
        const float most = std::min<float>(RateInHeat(self->_rate), UIScreen.mainScreen.maximumFramesPerSecond);
        self->_link.preferredFrameRateRange = CAFrameRateRangeMake(std::min(most, 60.0f), most, most);
    });
}

- (void)refreshed:(CADisplayLink*)link {
    _ticks.fetch_add(1, std::memory_order_relaxed);
    dispatch_semaphore_signal(_tick);
}

- (void)wait {
    // Not for ever: the caller also has to notice that it was asked to stop.
    dispatch_semaphore_wait(_tick, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 10));
    while (dispatch_semaphore_wait(_tick, DISPATCH_TIME_NOW) == 0) {}
}

- (unsigned)takeTicks { return _ticks.exchange(0, std::memory_order_relaxed); }

- (void)stop {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_link invalidate];
    _link = nil;
}
@end

namespace {
constexpr uint32_t defaultGuestWidth = 640;
constexpr uint32_t defaultGuestHeight = 960;
constexpr size_t displayPixelBudget = size_t(defaultGuestWidth) * defaultGuestHeight;

struct DisplaySize {
    uint32_t width;
    uint32_t height;
};

// How many pixels Android's display has. On an iPhone it is a fixed number, shaped like the view. An
// iPad's view is several times as large: with the same number of pixels everything on it was a
// phone's screen blown up. There Android gets two pixels for each square point of the view, about
// what an iPhone's view comes to, so things are the size they are on a phone and Android, finding
// itself on a wide display, lays its screens out as on a tablet.
size_t GuestPixelBudget(CGSize viewport) {
    if (UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPad) return displayPixelBudget;
    constexpr size_t most = 2400 * 1000;
    return std::clamp(size_t(viewport.width * viewport.height * 2.0), displayPixelBudget, most);
}

DisplaySize GuestDisplaySize(CGSize viewport) {
    if (viewport.width <= 0 || viewport.height <= 0) return {defaultGuestWidth, defaultGuestHeight};
    const double aspect = std::clamp(double(viewport.width / viewport.height), 0.25, 4.0);
    const double budget = double(GuestPixelBudget(viewport));
    const auto evenPixels = [](double length) { return uint32_t(2 * std::lround(length / 2)); };
    return {evenPixels(std::sqrt(budget * aspect)),
            evenPixels(std::sqrt(budget / aspect))};
}

UIButton* ToolbarButton(NSString* symbol, NSString* label, id target, SEL action) {
    UIButtonConfiguration* configuration = [UIButtonConfiguration plainButtonConfiguration];
    configuration.image = [UIImage systemImageNamed:symbol];
    configuration.baseForegroundColor = UIColor.whiteColor;
    UIButton* button = [UIButton buttonWithConfiguration:configuration primaryAction:nil];
    button.accessibilityLabel = label;
    [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    return button;
}

UIImage* GuestDisplayImage(std::unique_ptr<uint8_t[]>& pixels, size_t capacity,
                           uint32_t width, uint32_t height) {
    const size_t stride = size_t(width) * 4;
    const size_t bytes = stride * height;
    if (!width || !height || width > 4096 || height > 4096 || bytes > capacity) return nil;

    // Keep the readback alive through the image provider without copying it.
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, pixels.get(), bytes,
        [](void*, const void* data, size_t) { delete[] static_cast<const uint8_t*>(data); });
    if (!provider) return nil;
    pixels.release();
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGBitmapInfo bitmap = kCGBitmapByteOrder32Big | CGBitmapInfo(kCGImageAlphaNoneSkipLast);
    CGImageRef frame = color
        ? CGImageCreate(width, height, 8, 32, stride, color, bitmap, provider, nullptr, false,
                        kCGRenderingIntentDefault)
        : nullptr;
    UIImage* image = frame ? [UIImage imageWithCGImage:frame] : nil;
    if (frame) CGImageRelease(frame);
    if (color) CGColorSpaceRelease(color);
    CGDataProviderRelease(provider);
    return image;
}

std::jthread StartDisplayReader(const nestvane::GfxstreamSession& renderer, DisplaySize size, bool captureContent,
                                const std::atomic_bool& launcherDrawn,
                                void (^present)(id, CGSize, bool, bool)) {
    if (!renderer.hasDisplayReadback()) return {};
    return std::jthread([&renderer, &launcherDrawn, size, captureContent, present](std::stop_token stop) mutable {
        bool reportLauncher = true;
        // Frames come as surfaces copied on the GPU, taken when the screen is about to refresh. When the
        // renderer cannot make one, or NESTVANE_DISPLAY_READBACK=1 asks for the old way, pixels are
        // read back on a timer instead. NESTVANE_DISPLAY_FINISH=1 waits for the GPU after each copy.
        bool surfaces = renderer.hasDisplaySurface() && !getenv("NESTVANE_DISPLAY_READBACK");
        const bool finish = getenv("NESTVANE_DISPLAY_FINISH") != nullptr;
        unsigned misses = 0;
        // How often the display is looked at for a new frame: NESTVANE_DISPLAY_HZ, the rate the guest
        // composes at. NESTVANE_DISPLAY_RATE=1 writes what came of it to display-rate.txt each second:
        // the frames the guest composed and the frames that were shown.
        // NESTVANE_DISPLAY_READ_HZ looks at another rate than the guest composes at, to tell the two apart.
        const int hertz = std::clamp(std::atoi(getenv("NESTVANE_DISPLAY_HZ") ?: "30"), 10, 240);
        const int readHertz = std::clamp(std::atoi(getenv("NESTVANE_DISPLAY_READ_HZ") ?: "0") ?: hertz, 1, 240);
        const auto period = std::chrono::microseconds(1'000'000 / readHertz);
        __block ScreenRefresh* refresh = nil;
        if (surfaces) dispatch_sync(dispatch_get_main_queue(), ^{ refresh = [[ScreenRefresh alloc] initWithRate:readHertz]; });
        const bool reportRate = getenv("NESTVANE_DISPLAY_RATE") != nullptr;
        NSURL* rateURL = [[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject
            URLByAppendingPathComponent:@"display-rate.txt"];
        NSMutableString* rates = [NSMutableString string];
        auto rateStarted = std::chrono::steady_clock::now();
        uint64_t rateFirstFrame = 0;
        unsigned shown = 0;
        // The most frames composed in any tenth of the second: what the guest reaches while something moves.
        auto tenthStarted = rateStarted;
        uint64_t tenthFirst = 0, tenthMost = 0;
        auto next = std::chrono::steady_clock::now();
        const auto capacity = std::max(displayPixelBudget, size_t(size.width) * size.height) * 4;
        std::unique_ptr<uint8_t[]> pixels;
        const auto slot = dispatch_semaphore_create(1);
        uint64_t lastFrame = 0;
        while (!stop.stop_requested()) {
            @autoreleasepool {
                if (dispatch_semaphore_wait(slot, DISPATCH_TIME_NOW) == 0) {
                    const bool wantLauncher = reportLauncher && launcherDrawn.load(std::memory_order_relaxed);
                    uint64_t sequence = 0;
                    IOSurfaceRef surface = surfaces ? renderer.copyDisplaySurface(lastFrame, sequence, finish) : nullptr;
                    if (surface) {
                        if (!rateFirstFrame) rateFirstFrame = sequence;
                        ++shown;
                        lastFrame = sequence;
                        misses = 0;
                        const bool nonuniform = (captureContent || wantLauncher) && SurfaceHasColorVariation(surface);
                        const bool content = captureContent && nonuniform;
                        const bool launcher = wantLauncher && nonuniform;
                        if (content) captureContent = false;
                        if (launcher) reportLauncher = false;
                        const CGSize frameSize = CGSizeMake(IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface));
                        id frame = CFBridgingRelease(surface);
                        dispatch_async(dispatch_get_main_queue(), ^{
                            present(frame, frameSize, content, launcher);
                            dispatch_semaphore_signal(slot);
                        });
                    } else if (surfaces && (lastFrame || ++misses < 600)) {
                        // No newer frame, or every surface is on screen. Before the first frame this may
                        // also mean the renderer cannot make surfaces, so after a while pixels are read.
                        dispatch_semaphore_signal(slot);
                    } else {
                    if (surfaces) {
                        surfaces = false;
                        ScreenRefresh* ended = refresh;
                        refresh = nil;
                        dispatch_async(dispatch_get_main_queue(), ^{ [ended stop]; });
                    }
                    if (!pixels) pixels.reset(new (std::nothrow) uint8_t[capacity]);
                    const auto* readback = pixels.get();
                    uint32_t dimensions[2]{};
                    const auto frame = pixels
                        ? renderer.readDisplay(lastFrame, {pixels.get(), capacity}, dimensions)
                        : 0;
                    UIImage* image = frame
                        ? GuestDisplayImage(pixels, capacity, dimensions[0], dimensions[1])
                        : nil;
                    if (image) {
                        if (!rateFirstFrame) rateFirstFrame = frame;
                        ++shown;
                        lastFrame = frame;
                        const auto bytes = size_t(dimensions[0]) * dimensions[1] * 4;
                        const bool nonuniform = (captureContent || wantLauncher) &&
                            nestvane::guestDisplayHasColorVariation({readback, bytes});
                        const bool content = captureContent && nonuniform;
                        const bool launcher = wantLauncher && nonuniform;
                        if (content) captureContent = false;
                        if (launcher) reportLauncher = false;
                        const CGSize frameSize = image.size;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            present(image, frameSize, content, launcher);
                            dispatch_semaphore_signal(slot);
                        });
                    } else {
                        dispatch_semaphore_signal(slot);
                    }
                    }
                }
            }
            if (const auto now = std::chrono::steady_clock::now(); reportRate && now - tenthStarted >= std::chrono::milliseconds(100)) {
                tenthMost = std::max(tenthMost, lastFrame - tenthFirst);
                tenthFirst = lastFrame;
                tenthStarted = now;
            }
            if (const auto now = std::chrono::steady_clock::now(); reportRate && now - rateStarted >= std::chrono::seconds(1)) {
                const double seconds = std::chrono::duration<double>(now - rateStarted).count();
                static uint64_t soundBefore = 0;
                auto& sound = nestvane::GuestAudio::host();
                static uint64_t playedBefore = 0;
                const uint64_t soundNow = sound.taken.load(), playedNow = sound.played.load();
                rusage used{};
                getrusage(RUSAGE_SELF, &used);
                static double cpuBefore = 0;
                const double cpuNow = used.ru_utime.tv_sec + used.ru_stime.tv_sec + (used.ru_utime.tv_usec + used.ru_stime.tv_usec) / 1e6;
                [rates appendFormat:@"cpu %.0f%%; ", (cpuNow - std::exchange(cpuBefore, cpuNow)) / seconds * 100];
                [rates appendFormat:@"%@%dx%d at %d Hz: composed %.0f, shown %.0f frames per second; sound %.0f bytes a second taken, %.0f played by Android, peak %u (%@, %zu bytes waiting, %llu short); most in a tenth %llu; %s\n",
                    refresh ? [NSString stringWithFormat:@"surfaces, screen refreshed %.0f times a second; ", [refresh takeTicks] / seconds] : @"read back; ",
                    size.width, size.height, hertz,
                    rateFirstFrame ? double(lastFrame - rateFirstFrame + 1) / seconds : 0.0, shown / seconds, double(soundNow - soundBefore) / seconds, double(playedNow - playedBefore) / seconds,
                    sound.peak.exchange(0), GuestAudioOutputState, sound.buffered(), sound.shortfalls.exchange(0), std::exchange(tenthMost, 0),
                    nestvane::IOSVideoDecoderReport().c_str()];
                {
                    auto& microphone = nestvane::GuestMicrophone::host();
                    static uint64_t givenBefore = 0;
                    const uint64_t givenNow = microphone.given.load();
                    if (givenNow || microphone.wanted.load())
                        [rates appendFormat:@"microphone %@: %.0f bytes a second to Android, peak %u\n", GuestMicrophoneState,
                            double(givenNow - std::exchange(givenBefore, givenNow)) / seconds, microphone.peak.exchange(0)];
                }
                soundBefore = soundNow;
                playedBefore = playedNow;
                [rates writeToURL:rateURL atomically:YES encoding:NSUTF8StringEncoding error:nil];
                rateStarted = now;
                rateFirstFrame = 0;
                shown = 0;
            }
            if (refresh) {
                [refresh wait];
            } else {
                next = std::max(next + period, std::chrono::steady_clock::now());
                std::this_thread::sleep_until(next);
            }
        }
        if (ScreenRefresh* ended = refresh) dispatch_async(dispatch_get_main_queue(), ^{ [ended stop]; });
    });
}
void WriteGuestDisplayPNG(NSData* png, NSURL* destination) {
    NSError* error = nil;
    if (!png || ![png writeToURL:destination options:NSDataWritingAtomic error:&error])
        NSLog(@"Unable to export display capture: %@", error);
}

// For measuring whether Android could fit a free developer account's address space (PLAN.md, "先量 2.0"):
// every host region, summed by what it is. File mappings go by file name, the rest by their VM tag;
// untagged anonymous memory is split into what can be accessed and what is only reserved (no access:
// guard pages and the address space the runtime holds for later).
void AppendAddressMap(std::string& report) {
    struct Sum { uint64_t bytes = 0, resident = 0; unsigned regions = 0; };
    std::map<std::string, Sum> sums;
    uint64_t highest = 0, total = 0;
    unsigned regions = 0;
    const auto started = std::chrono::steady_clock::now();
    bool timedOut = false;
    uint64_t namingMs = 0;  // microseconds, despite the name until printed
    std::map<uint32_t, std::string> names;
    vm_address_t address = 0;
    for (;;) {
        if ((++regions & 15) == 0 && std::chrono::steady_clock::now() - started > std::chrono::seconds(240)) {
            timedOut = true;
            break;
        }
        vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t infoCount = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (vm_region_recurse_64(mach_task_self(), &address, &size, &depth,
                                   reinterpret_cast<vm_region_recurse_info_t>(&info), &infoCount) != KERN_SUCCESS)
            break;
        char path[1024] = {};
        std::string key;
        // A submap is the system's shared code (dyld's cache): counted whole, not walked into.
        if (info.is_submap) {
            key = "submap";
        } else if (info.protection != VM_PROT_NONE && info.share_mode != SM_EMPTY && [&] {
                       if (const auto known = names.find(info.object_id); known != names.end()) {
                           key = known->second;
                           return !key.empty();
                       }
                       const auto naming = std::chrono::steady_clock::now();
                       std::string name;
                       if (proc_regionfilename(getpid(), address, path, sizeof(path)) > 0) {
                           std::string_view whole(path);
                           if (const auto slash = whole.rfind('/'); slash != std::string_view::npos) whole.remove_prefix(slash + 1);
                           name = "file " + std::string(whole);
                       }
                       namingMs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - naming).count();
                       if (info.object_id) names[info.object_id] = name;
                       key = name;
                       return !key.empty();
                   }()) {
        } else if (info.user_tag) {
            key = "tag " + std::to_string(info.user_tag);
        } else {
            key = info.protection == VM_PROT_NONE ? "anonymous, no access" : "anonymous";
        }
        auto& sum = sums[key];
        sum.bytes += size;
        sum.resident += uint64_t(info.pages_resident) * vm_page_size;
        ++sum.regions;
        total += size;
        highest = std::max<uint64_t>(highest, address + size);
        address += size;
    }
    std::vector<std::pair<std::string, Sum>> sorted(sums.begin(), sums.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    report += "  address map: " + std::to_string(total) + " bytes in " + std::to_string(regions) + " regions, highest address " +
              std::to_string(highest) + ", " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started).count()) + " ms" + (timedOut ? ", stopped at 240 s" : "") + ", " + std::to_string(namingMs / 1000) + " ms naming files" + "\n";
    size_t shown = 0;
    for (const auto& [key, sum] : sorted) {
        if (++shown > 60) break;
        report += "    " + std::to_string(sum.bytes) + " bytes, " + std::to_string(sum.resident) + " resident, " +
                  std::to_string(sum.regions) + " regions: " + key + "\n";
    }
}

void AppendMemoryUsage(std::string& report) {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        report += "  peak iOS process RSS bytes: " + std::to_string(usage.ru_maxrss) + "\n";
    task_vm_info_data_t memory{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&memory), &count) != KERN_SUCCESS)
        return;
    if (count >= TASK_VM_INFO_REV1_COUNT)
        report += "  iOS physical footprint bytes: " + std::to_string(memory.phys_footprint) + "\n";
    if (count >= TASK_VM_INFO_REV3_COUNT)
        report += "  peak iOS physical footprint bytes: " + std::to_string(memory.ledger_phys_footprint_peak) + "\n";
    if (count >= TASK_VM_INFO_REV4_COUNT)
        report += "  iOS process memory limit remaining bytes: " + std::to_string(memory.limit_bytes_remaining) + "\n";
    // What the footprint is made of: anonymous memory, the compressed part of it, and file pages.
    report += "  iOS memory bytes: internal " + std::to_string(memory.internal) + ", compressed " +
              std::to_string(memory.compressed) + ", external " + std::to_string(memory.external) +
              ", reusable " + std::to_string(memory.reusable) + ", resident " + std::to_string(memory.resident_size) + "\n";
    if (count >= TASK_VM_INFO_REV1_COUNT && count >= TASK_VM_INFO_REV2_COUNT)
        report += "  iOS address range: " + std::to_string(memory.min_address) + " to " + std::to_string(memory.max_address) +
                  ", virtual size " + std::to_string(memory.virtual_size) + "\n";
}
} // namespace

// The rate Android composes at and the app looks for frames at: 60 a second, or 120 on a screen that
// shows 120 when the person has turned that on in Settings. The framework draws at 120 only because
// the overlay in fixtures/overlay-framework lifts its default of 60. NESTVANE_DISPLAY_HZ, when the app
// was launched with it, overrides both; Android reads the rate once, as it starts.
static NSString* const Display120HzKey = @"Display120Hz";
// Built-in root: whether it is on (from the next start of Android), and each app's answer, kept as
// RootGrantPrefix + package name. Apps are asked the first time they start su.
static NSString* const RootEnabledKey = @"RootEnabled";
// On an iPad: Android is given a tablet's density, and lays its screens out as on a tablet (1.16, off by default).
static NSString* const TabletLayoutKey = @"TabletLayout";
static NSString* const RootGrantPrefix = @"RootGrant:";
static NSString* const RootProfilePrefix = @"RootProfile:";
static NSInteger GuestDisplayRate() {
    static const NSInteger launchedWith = std::atoi(getenv("NESTVANE_DISPLAY_HZ") ?: "");
    if (launchedWith) return launchedWith;
    const NSInteger screen = UIScreen.mainScreen.maximumFramesPerSecond;
    return std::min<NSInteger>(screen, [NSUserDefaults.standardUserDefaults boolForKey:Display120HzKey] ? 120 : 60);
}

// The update notice. Once a day the app asks GitHub for the tag of the latest release and remembers it;
// when that is a later version than this one, the start screen says so and Settings links to it. The
// app installs nothing itself. "Check for updates" in Settings turns the question off.
// NESTVANE_TEST_UPDATE=<version> asks at once and compares as if the app were that version.
static NSString* const UpdateCheckKey = @"UpdateCheck";
static NSString* const UpdateLatestKey = @"UpdateLatestVersion";
static NSString* const UpdateAskedKey = @"UpdateAskedAt";
static NSString* const UpdateToldKey = @"UpdateToldVersion";  // the version the launch notice last named
static NSString* const UpdateReleaseAddress = @"https://github.com/atomsprout/nestvane/releases/latest";
static NSString* AvailableUpdate() {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    NSString* latest = [defaults stringForKey:UpdateLatestKey];
    NSString* current = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_UPDATE"]
        ?: NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
    if (![defaults boolForKey:UpdateCheckKey] || !latest.length || !current.length) return nil;
    return [latest compare:current options:NSNumericSearch] == NSOrderedDescending ? latest : nil;
}
static void AskForUpdate(void (^found)(NSString* version)) {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{UpdateCheckKey: @YES}];
    const BOOL test = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_UPDATE"] != nil;
    const NSTimeInterval now = NSDate.date.timeIntervalSince1970, asked = [defaults doubleForKey:UpdateAskedKey];
    if (![defaults boolForKey:UpdateCheckKey]) return;
    if (!test && asked > 0 && now - asked < 24 * 3600 && now >= asked) {
        if (NSString* version = AvailableUpdate()) found(version);
        return;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:
        [NSURL URLWithString:@"https://api.github.com/repos/atomsprout/nestvane/releases/latest"]
        cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:15];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData* data, NSURLResponse* response, NSError*) {
        NSDictionary* release = data && static_cast<NSHTTPURLResponse*>(response).statusCode == 200
            ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        NSString* tag = [release isKindOfClass:NSDictionary.class] ? release[@"tag_name"] : nil;
        if (![tag isKindOfClass:NSString.class] || tag.length < 2 || tag.length > 32) return;  // asked again at the next launch
        if ([tag hasPrefix:@"v"]) tag = [tag substringFromIndex:1];
        // A version is digits and dots: nothing else from the network is shown or stored.
        if ([tag rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789."].invertedSet].location != NSNotFound) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [defaults setObject:tag forKey:UpdateLatestKey];
            [defaults setDouble:now forKey:UpdateAskedKey];
            if (NSString* version = AvailableUpdate()) found(version);
        });
    }] resume];
}

static NSString* ThermalState() {
    switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateNominal: return @"nominal";
        case NSProcessInfoThermalStateFair: return @"fair";
        case NSProcessInfoThermalStateSerious: return @"serious";
        case NSProcessInfoThermalStateCritical: return @"critical";
    }
    return @"unknown";
}

// "Submit a bug" in the More menu opens a new issue on GitHub with the things every report needs
// already written in: the version of Nestvane, the version of iOS, the model of the phone, its
// memory, and whether 120 Hz is on. Nothing that names the phone or its owner. The person sees the
// text in the browser and decides whether to submit it; the app sends nothing itself.
static NSURL* BugReportAddress(bool androidRunning) {
    utsname system{};
    uname(&system);
    NSDictionary* info = NSBundle.mainBundle.infoDictionary;
    NSString* body = [NSString stringWithFormat:
        @"**What happened**\n\n<!-- What you did, what you expected, and what you saw instead. -->\n\n"
        "**The app that shows it, if it is about one**\n\n\n"
        "**System**\n\n- Nestvane %@ (%@)\n- iOS %@\n- %s\n- %.0f GB of memory\n- 120 Hz %@\n- Android %@\n- Phone temperature: %@\n",
        info[@"CFBundleShortVersionString"], info[@"CFBundleVersion"], UIDevice.currentDevice.systemVersion,
        system.machine, std::round(NSProcessInfo.processInfo.physicalMemory / 1e9),
        [NSUserDefaults.standardUserDefaults boolForKey:Display120HzKey] ? @"on" : @"off",
        androidRunning ? @"was running" : @"was not running", ThermalState()];
    NSURLComponents* address = [NSURLComponents componentsWithString:@"https://github.com/atomsprout/nestvane/issues/new"];
    address.queryItems = @[[NSURLQueryItem queryItemWithName:@"body" value:body]];
    return address.URL;
}

// Android's display. A frame is an IOSurface the renderer copied on the GPU, given to the layer as it
// is, or, when the renderer cannot do that, an image made of pixels read back.
@interface GuestDisplayView : UIView
// The frame on screen as an image, for captures; nil when there is none.
@property (nonatomic, readonly) UIImage* image;
- (void)show:(id)frame size:(CGSize)size;
// Which way up Android holds its display, in quarter turns (GuestApkInbox::rotation). The frames are
// always the display as it was made; this turns the picture, and the touches, to match.
- (void)setRotation:(int)quarterTurns;
- (void)tapAtX:(int)x y:(int)y;
- (void)setTouchDevice:(std::shared_ptr<nestvane::GuestTouch>)device;
- (void)pressKey:(nestvane::GuestTouch::Key)key;
@end

@implementation GuestDisplayView {
    std::shared_ptr<nestvane::GuestTouch> _touchDevice;
    UITouch* _activeTouch;
    id _frame;
    CGSize _frameSize;
    CALayer* _content;  // the picture: as large as fits, turned the way Android's display is turned
    int _rotation;
}

- (void)show:(id)frame size:(CGSize)size {
    const CGSize before = _frameSize;
    _frame = frame;
    _frameSize = frame ? size : CGSizeZero;
    if (!_content) {
        _content = [CALayer layer];
        _content.contentsGravity = kCAGravityResize;
        [self.layer addSublayer:_content];
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _content.contents = [frame isKindOfClass:UIImage.class] ? (__bridge id)static_cast<UIImage*>(frame).CGImage : frame;
    [CATransaction commit];
    if (!CGSizeEqualToSize(before, _frameSize)) [self placeContent];
}

- (void)setRotation:(int)quarterTurns {
    if (quarterTurns == _rotation) return;
    _rotation = quarterTurns & 3;
    [self placeContent];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [self placeContent];
}

// Android turned a quarter to the left shows its screens with their top along the right edge of the
// frame, so the picture is turned a quarter to the left to stand them up, and so on round.
- (void)placeContent {
    if (!_content) return;
    const CGRect bounds = self.bounds;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _content.affineTransform = CGAffineTransformIdentity;
    if (_frameSize.width > 0 && _frameSize.height > 0 && bounds.size.width > 0 && bounds.size.height > 0) {
        const bool turned = _rotation & 1;
        const CGFloat acrossWidth = turned ? _frameSize.height : _frameSize.width;
        const CGFloat acrossHeight = turned ? _frameSize.width : _frameSize.height;
        const CGFloat scale = std::min(bounds.size.width / acrossWidth, bounds.size.height / acrossHeight);
        _content.bounds = CGRectMake(0, 0, _frameSize.width * scale, _frameSize.height * scale);
        _content.position = CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds));
        _content.affineTransform = CGAffineTransformMakeRotation(-M_PI_2 * _rotation);
    } else {
        _content.frame = bounds;
    }
    [CATransaction commit];
}

- (UIImage*)image {
    if ([_frame isKindOfClass:UIImage.class]) return _frame;
    return _frame ? ImageOfSurface((__bridge IOSurfaceRef)_frame) : nil;
}

- (void)setTouchDevice:(std::shared_ptr<nestvane::GuestTouch>)device {
    if (_activeTouch) [self sendTouch:_activeTouch down:NO];
    _activeTouch = nil;
    _touchDevice = std::move(device);
}

- (void)pressKey:(nestvane::GuestTouch::Key)key {
    if (_touchDevice) _touchDevice->press(key);
}

- (BOOL)sendTouch:(UITouch*)touch down:(BOOL)down {
    return [self sendLocation:[touch locationInView:self] down:down];
}

// For tests: a tap at a place in this view, in thousandths of its width and height, sent the way a
// finger's touch is.
- (void)tapAtX:(int)x y:(int)y {
    const CGPoint place = CGPointMake(self.bounds.size.width * x / 1000, self.bounds.size.height * y / 1000);
    UITouch* held = _activeTouch;
    _activeTouch = nil;
    if ([self sendLocation:place down:YES]) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            [self sendLocation:place down:NO];
        });
    }
    _activeTouch = held;
}

- (BOOL)sendLocation:(CGPoint)place down:(BOOL)down {
    const CGSize imageSize = _frameSize;
    const CGRect bounds = self.bounds;
    if (!_touchDevice || imageSize.width <= 0 || imageSize.height <= 0 ||
        bounds.size.width <= 0 || bounds.size.height <= 0) return NO;
    // The touch device's coordinates are those of the display as it was made, which is the picture's
    // own: the layer takes the turn out.
    const CGSize size = _content.bounds.size;
    if (size.width <= 0 || size.height <= 0) return NO;
    const CGPoint location = [_content convertPoint:place fromLayer:self.layer];
    const CGFloat x = location.x / size.width;
    const CGFloat y = location.y / size.height;
    if (!_activeTouch && (x < 0 || x > 1 || y < 0 || y > 1)) return NO;
    const auto axis = [](CGFloat location) {
        return int32_t(std::clamp(location, CGFloat(0), CGFloat(1)) * nestvane::GuestTouch::maximumAxis + 0.5);
    };
    return _touchDevice->submit(axis(x), axis(y), down);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_activeTouch) return;
    UITouch* touch = touches.anyObject;
    if ([self sendTouch:touch down:YES]) _activeTouch = touch;
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_activeTouch && [touches containsObject:_activeTouch] && ![self sendTouch:_activeTouch down:YES])
        _activeTouch = nil;
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_activeTouch || ![touches containsObject:_activeTouch]) return;
    [self sendTouch:_activeTouch down:NO];
    _activeTouch = nil;
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self touchesEnded:touches withEvent:event];
}
@end

static NSString* const InstallApkShortcut = @"com.atomsprout.nestvane.install-apk";

// Android's sound, played by the phone: an output unit takes what the guest's sound device wrote
// (runtime/GuestAudio.hpp), in the format the guest plays in, and plays silence when there is nothing.
static OSStatus RenderGuestAudio(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList* output) {
    for (UInt32 index = 0; index < output->mNumberBuffers; ++index) {
        auto* bytes = static_cast<uint8_t*>(output->mBuffers[index].mData);
        const size_t wanted = output->mBuffers[index].mDataByteSize;
        const size_t got = index ? 0 : nestvane::GuestAudio::host().read(bytes, wanted);
        std::memset(bytes + got, 0, wanted - got);
    }
    return noErr;
}

// Whether the microphone is recording for Android: the audio session is then one that plays and records.
static bool GuestMicrophoneRecording = false;

// Follows the guest's recording: the phone's microphone is opened when Android reads from its capture
// device, in the format it set, and closed three seconds after it last did. iOS asks the person for the
// microphone the first time; until they allow it, and if they do not, Android records silence.
static void FollowGuestMicrophone() {
    static AVAudioEngine* engine = nil;
    static uint64_t recording = 0;
    static bool asking = false, refused = false;
    auto& microphone = nestvane::GuestMicrophone::host();
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t last = microphone.wanted.load(), generation = microphone.generation.load();
    const bool wanted = last && now - last < 3000 && microphone.rate.load();
    // When the input changes under a recording (another app's sound, headphones, a call), the engine
    // stops and its tap's format is stale: no more sound came, and Android's recording stood still or
    // went on at the old rate. It is made again in the input's new format.
    static std::atomic_bool changed{false};
    static id observer = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioEngineConfigurationChangeNotification
        object:nil queue:nil usingBlock:^(NSNotification*) { changed = true; }];
    (void)observer;
    if (engine && changed.exchange(false)) recording = ~uint64_t(0);
    if (engine && (!wanted || generation != recording)) {
        [engine.inputNode removeTapOnBus:0];
        [engine stop];
        engine = nil;
        microphone.producer = false;
        GuestMicrophoneRecording = false;
        GuestMicrophoneState = @"idle";
        [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:nil];
    }
    if (!wanted || engine || asking || refused) return;
    const AVAudioApplicationRecordPermission permission = AVAudioApplication.sharedInstance.recordPermission;
    if (permission == AVAudioApplicationRecordPermissionUndetermined) {
        asking = true;
        GuestMicrophoneState = @"asking";
        [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
            dispatch_async(dispatch_get_main_queue(), ^{ asking = false; refused = !granted; });
        }];
        return;
    }
    if (permission != AVAudioApplicationRecordPermissionGranted) {
        refused = true;
        GuestMicrophoneState = @"not allowed";
        return;
    }
    static CFAbsoluteTime retry = 0;
    if (CFAbsoluteTimeGetCurrent() < retry) return;
    retry = CFAbsoluteTimeGetCurrent() + 2;
    NSError* error = nil;
    if (![AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayAndRecord
                                        withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetooth
                                              error:&error] ||
        ![AVAudioSession.sharedInstance setActive:YES error:&error]) {
        GuestMicrophoneState = [NSString stringWithFormat:@"audio session: %@", error.localizedDescription];
        return;
    }
    AVAudioEngine* made = [AVAudioEngine new];
    AVAudioFormat* heard = [made.inputNode outputFormatForBus:0];
    const uint32_t bits = microphone.bits.load();
    AVAudioFormat* asked = [[AVAudioFormat alloc] initWithCommonFormat:bits == 16 ? AVAudioPCMFormatInt16 : AVAudioPCMFormatInt32
                                                            sampleRate:microphone.rate.load()
                                                              channels:microphone.channels.load()
                                                           interleaved:YES];
    AVAudioConverter* converter = heard.sampleRate > 0 && asked ? [[AVAudioConverter alloc] initFromFormat:heard toFormat:asked] : nil;
    if (!converter) {
        GuestMicrophoneState = @"no input format";
        [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:nil];
        return;
    }
    // AVAudioEngine reports a format it cannot tap by raising an exception, which must not end the app
    // and Android with it: without the microphone Android goes on recording silence.
    @try {
    [made.inputNode installTapOnBus:0 bufferSize:1024 format:heard block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
        const AVAudioFrameCount room = AVAudioFrameCount(buffer.frameLength * asked.sampleRate / heard.sampleRate) + 64;
        AVAudioPCMBuffer* out = [[AVAudioPCMBuffer alloc] initWithPCMFormat:asked frameCapacity:room];
        __block BOOL given = NO;
        [converter convertToBuffer:out error:nil withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount, AVAudioConverterInputStatus* status) {
            *status = given ? AVAudioConverterInputStatus_NoDataNow : AVAudioConverterInputStatus_HaveData;
            if (given) return nil;
            given = YES;
            return buffer;
        }];
        if (out.frameLength)
            nestvane::GuestMicrophone::host().write(static_cast<const uint8_t*>(out.audioBufferList->mBuffers[0].mData),
                                                    size_t(out.frameLength) * asked.streamDescription->mBytesPerFrame);
    }];
    if (![made startAndReturnError:&error]) {
        [made.inputNode removeTapOnBus:0];
        GuestMicrophoneState = [NSString stringWithFormat:@"starting: %@", error.localizedDescription];
        [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:nil];
        return;
    }
    } @catch (NSException* exception) {
        GuestMicrophoneState = [NSString stringWithFormat:@"refused: %@", exception.reason];
        refused = true;
        [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:nil];
        return;
    }
    engine = made;
    recording = generation;
    GuestMicrophoneRecording = true;
    microphone.producer = true;
    GuestMicrophoneState = [NSString stringWithFormat:@"recording %u Hz, %u channels, %u bits from %.0f Hz, %u channels",
                            microphone.rate.load(), microphone.channels.load(), bits, heard.sampleRate, heard.channelCount];
}

// Follows the guest's playing: started when the guest plays, in the format it set, and again when that changes.
static void FollowGuestAudio() {
    static AudioUnit unit = nullptr;
    static uint64_t playing = 0;
    static uint64_t played = 0;
    static CFAbsoluteTime heard = 0;
    auto& audio = nestvane::GuestAudio::host();
    const uint64_t generation = audio.generation.load();
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (const uint64_t count = audio.played.load(); count != played) {
        played = count;
        heard = now;
    }
    // Android keeps its sound device only while something plays. Three seconds after the last of it the
    // phone's output is given back, so that other apps' sound can go on, and taken again when Android plays.
    const bool wanted = now - heard < 3;
    if (generation == playing && wanted == (unit != nullptr)) return;
    static CFAbsoluteTime retry = 0;  // an output that would not start is tried again two seconds later
    if (generation == playing && wanted && now < retry) return;
    playing = generation;
    audio.consumer = false;
    if (unit) {
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        unit = nullptr;
        if (!wanted && !GuestMicrophoneRecording) {
            [AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nil];
            GuestAudioOutputState = @"idle";
        }
    }
    if (!wanted) return;
    retry = now + 2;
    const uint32_t bits = audio.bits.load(), channels = audio.channels.load(), rate = audio.rate.load();
    if (!rate || !channels) return;
    // Each step that can refuse, by name, for the rate report (NESTVANE_DISPLAY_RATE).
    const auto step = [](const char* name, OSStatus status) {
        if (status != noErr) GuestAudioOutputState = [NSString stringWithFormat:@"%s failed with %d", name, int(status)];
        return status == noErr;
    };
    NSError* error = nil;
    if ((!GuestMicrophoneRecording && ![AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:&error]) ||
        ![AVAudioSession.sharedInstance setActive:YES error:&error]) {
        GuestAudioOutputState = [NSString stringWithFormat:@"audio session: %@", error.localizedDescription];
        return;
    }
    AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (!component) { GuestAudioOutputState = @"no output unit"; return; }
    if (!step("creating the output unit", AudioComponentInstanceNew(component, &unit))) { unit = nullptr; return; }
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = bits;
    format.mBytesPerFrame = channels * bits / 8;
    format.mBytesPerPacket = format.mBytesPerFrame;
    format.mFramesPerPacket = 1;
    AURenderCallbackStruct callback{RenderGuestAudio, nullptr};
    if (!step("setting the format", AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format))) ||
        !step("setting the callback", AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback))) ||
        !step("initializing", AudioUnitInitialize(unit)) || !step("starting", AudioOutputUnitStart(unit))) {
        AudioComponentInstanceDispose(unit);
        unit = nullptr;
        return;
    }
    GuestAudioOutputState = [NSString stringWithFormat:@"playing %u Hz, %u channels, %u bits", rate, channels, bits];
    audio.consumer = true;
}

// What the start screen shows above its status line: the app's icon, glowing, with sparks in the
// icon's colours drifting up from around it. Still when the system asks for reduced motion, and
// when the start screen is not showing.
@interface EmblemView : UIView
@property(nonatomic) BOOL active;
@end

@implementation EmblemView {
    CAEmitterLayer* _sparks;
    CALayer* _glow;
    UIImageView* _icon;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.translatesAutoresizingMaskIntoConstraints = NO;
    UIImage* dot = [[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(24, 24)]
        imageWithActions:^(UIGraphicsImageRendererContext* context) {
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            const CGFloat colors[] = {1, 1, 1, 1, 1, 1, 1, 0};
            CGGradientRef gradient = CGGradientCreateWithColorComponents(space, colors, nullptr, 2);
            CGContextDrawRadialGradient(context.CGContext, gradient, CGPointMake(12, 12), 0, CGPointMake(12, 12), 12, 0);
            CGGradientRelease(gradient);
            CGColorSpaceRelease(space);
        }];
    _sparks = [CAEmitterLayer layer];
    _sparks.emitterShape = kCAEmitterLayerCircle;
    _sparks.emitterMode = kCAEmitterLayerOutline;
    _sparks.renderMode = kCAEmitterLayerAdditive;
    NSMutableArray<CAEmitterCell*>* cells = [NSMutableArray array];
    for (UIColor* color in @[[UIColor colorWithRed:0.25 green:0.90 blue:1.00 alpha:1], [UIColor colorWithRed:1.00 green:0.35 blue:0.85 alpha:1],
                             [UIColor colorWithRed:1.00 green:0.62 blue:0.20 alpha:1], [UIColor colorWithRed:0.62 green:0.45 blue:1.00 alpha:1]]) {
        CAEmitterCell* cell = [CAEmitterCell emitterCell];
        cell.contents = (__bridge id)dot.CGImage;
        cell.color = color.CGColor;
        cell.birthRate = 13;
        cell.lifetime = 2.6;
        cell.lifetimeRange = 1.1;
        cell.velocity = 34;
        cell.velocityRange = 26;
        cell.emissionRange = 2 * M_PI;
        cell.yAcceleration = -22;
        cell.scale = 0.26;
        cell.scaleRange = 0.2;
        cell.scaleSpeed = -0.05;
        cell.alphaSpeed = -0.36;
        [cells addObject:cell];
    }
    _sparks.emitterCells = cells;
    _sparks.birthRate = 0;
    [self.layer addSublayer:_sparks];
    _glow = [CALayer layer];
    _glow.backgroundColor = [UIColor colorWithRed:0.45 green:0.35 blue:1.0 alpha:1].CGColor;
    _glow.cornerRadius = 27;
    _glow.shadowColor = [UIColor colorWithRed:0.55 green:0.45 blue:1.0 alpha:1].CGColor;
    _glow.shadowOffset = CGSizeZero;
    _glow.shadowRadius = 26;
    _glow.shadowOpacity = 0.75;
    [self.layer addSublayer:_glow];
    _icon = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"Emblem"]];
    _icon.translatesAutoresizingMaskIntoConstraints = NO;
    _icon.layer.cornerRadius = 27;
    _icon.layer.cornerCurve = kCACornerCurveContinuous;
    _icon.clipsToBounds = YES;
    _icon.isAccessibilityElement = NO;
    [self addSubview:_icon];
    [NSLayoutConstraint activateConstraints:@[
        [self.heightAnchor constraintEqualToConstant:230],
        [_icon.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [_icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_icon.widthAnchor constraintEqualToConstant:120],
        [_icon.heightAnchor constraintEqualToConstant:120],
    ]];
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const CGPoint center = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    _sparks.frame = self.bounds;
    _sparks.emitterPosition = center;
    _sparks.emitterSize = CGSizeMake(66, 66);
    _glow.frame = CGRectMake(center.x - 58, center.y - 58, 116, 116);
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    self.active = _active;
}

- (void)setActive:(BOOL)active {
    _active = active;
    const BOOL moving = active && self.window && !UIAccessibilityIsReduceMotionEnabled();
    _sparks.birthRate = moving ? 1 : 0;
    [_glow removeAllAnimations];
    [_icon.layer removeAllAnimations];
    if (!moving) return;
    CABasicAnimation* pulse = [CABasicAnimation animationWithKeyPath:@"shadowRadius"];
    pulse.fromValue = @18;
    pulse.toValue = @38;
    pulse.duration = 1.6;
    pulse.autoreverses = YES;
    pulse.repeatCount = HUGE_VALF;
    pulse.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [_glow addAnimation:pulse forKey:@"pulse"];
    CABasicAnimation* breathe = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
    breathe.fromValue = @0.97;
    breathe.toValue = @1.03;
    breathe.duration = 1.6;
    breathe.autoreverses = YES;
    breathe.repeatCount = HUGE_VALF;
    breathe.timingFunction = pulse.timingFunction;
    [_icon.layer addAnimation:breathe forKey:@"breathe"];
}
@end

// The running Android's request channel, for asking it the names of its apps (GuestApkInbox::submitLabels).
static std::weak_ptr<nestvane::GuestApkInbox> RunningInbox;
static NSString* const RootLabelPrefix = @"RootLabel:";

// The name of an app, as Android last said: from the running Android, else as remembered.
static NSString* AppLabel(NSString* package) {
    const auto label = nestvane::GuestApkInbox::label(package.UTF8String);
    if (!label.empty()) {
        NSString* name = @(label.c_str());
        [NSUserDefaults.standardUserDefaults setObject:name forKey:[RootLabelPrefix stringByAppendingString:package]];
        return name;
    }
    return [NSUserDefaults.standardUserDefaults stringForKey:[RootLabelPrefix stringByAppendingString:package]];
}

static NSString* RootFooter() {
    return @"Apps in Android can then ask for root with su, and each one is asked about once. An app with root can read and change "
           @"every app's data. It takes effect the next time Android starts. Below are the apps that have asked; turn one off to refuse it.";
}

// A switch for each app that has asked for root.
static NSArray<NSDictionary*>* RootGrantRows() {
    NSMutableArray* rows = [NSMutableArray array];
    NSArray* keys = [NSUserDefaults.standardUserDefaults.dictionaryRepresentation.allKeys sortedArrayUsingSelector:@selector(compare:)];
    for (NSString* key in keys) {
        if (![key hasPrefix:RootGrantPrefix]) continue;
        NSString* package = [key substringFromIndex:RootGrantPrefix.length];
        NSString* name = AppLabel(package);
        [rows addObject:@{@"symbol": @"app", @"title": name ?: package, @"detail": name ? package : @"",
                          @"url": @"", @"switch": key}];
    }
    return rows;
}

// The settings that belong to Android rather than to the app, which a backup carries: whether root
// is on, each app's answer and KernelSU profile, and the apps' names.
static bool AndroidSetting(NSString* key) {
    return [key isEqualToString:RootEnabledKey] || [key hasPrefix:RootGrantPrefix] || [key hasPrefix:RootProfilePrefix] ||
           [key hasPrefix:RootLabelPrefix];
}
static NSDictionary* AndroidSettings() {
    NSMutableDictionary* settings = [NSMutableDictionary dictionary];
    [NSUserDefaults.standardUserDefaults.dictionaryRepresentation enumerateKeysAndObjectsUsingBlock:^(NSString* key, id value, BOOL*) {
        if (AndroidSetting(key)) settings[key] = value;
    }];
    return settings;
}
static void ApplyAndroidSettings(NSDictionary* settings) {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    for (NSString* key in defaults.dictionaryRepresentation.allKeys)
        if (AndroidSetting(key)) [defaults removeObjectForKey:key];
    [settings enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL*) {
        if ([key isKindOfClass:NSString.class] && AndroidSetting(key) &&
            ([value isKindOfClass:NSNumber.class] || [value isKindOfClass:NSString.class]))
            [defaults setObject:value forKey:key];
    }];
}

// Asks the person whether an app may have root, once at a time for each app. The answer is kept.
static void AskForRoot(NSString* name) {
    static NSMutableSet<NSString*>* asking = [NSMutableSet set];
    @synchronized (asking) {
        if ([asking containsObject:name]) return;
        [asking addObject:name];
    }
    // Android is asked for the app's name, which takes it a second or two; the prompt waits that long.
    if (auto inbox = RunningInbox.lock(); inbox && !AppLabel(name)) inbox->submitLabels();
    const auto delay = AppLabel(name) ? 0 : 2 * NSEC_PER_SEC;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay)), dispatch_get_main_queue(), ^{
        UIViewController* top = nil;
        for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow* window in static_cast<UIWindowScene*>(scene).windows)
                if (window.isKeyWindow) top = window.rootViewController;
        }
        while (top.presentedViewController) top = top.presentedViewController;
        const auto answer = ^(BOOL allowed) {
            [NSUserDefaults.standardUserDefaults setBool:allowed forKey:[RootGrantPrefix stringByAppendingString:name]];
            @synchronized (asking) { [asking removeObject:name]; }
        };
        UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Allow root?"
            message:[NSString stringWithFormat:@"%@ asks for root. With root it can read and change every app's data in Android. "
                                                @"You can change this later in Settings.",
                                                AppLabel(name) ? [NSString stringWithFormat:@"%@ (%@)", AppLabel(name), name] : name]
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Don't Allow" style:UIAlertActionStyleCancel
                                                handler:^(UIAlertAction*) { answer(NO); }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Allow" style:UIAlertActionStyleDestructive
                                                handler:^(UIAlertAction*) { answer(YES); }]];
        if (top) [top presentViewController:alert animated:YES completion:nil];
        else @synchronized (asking) { [asking removeObject:name]; }
    });
}

// The app's settings: what it is and which version, where Android's shared files are, and where to
// find the people who make it. Reached from the More menu.
@interface SettingsViewController : UITableViewController
@end

@implementation SettingsViewController {
    NSArray<NSDictionary*>* _sections;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Settings";
    // The names of the apps in the root list come from Android: ask it again, for the next time.
    if (auto inbox = RunningInbox.lock()) inbox->submitLabels();
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                           target:self action:@selector(close)];
    NSURL* storage = [[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject
        URLByAppendingPathComponent:@"Android"];
    NSString* files = [@"shareddocuments://" stringByAppendingString:
        [storage.path stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet]];
    NSString* rate = [NSString stringWithFormat:@"%s frames per second", getenv("NESTVANE_DISPLAY_HZ") ?: "60"];
    const BOOL fast = UIScreen.mainScreen.maximumFramesPerSecond >= 120;
    // The tablet layout is offered where the display is large enough for it.
    const BOOL tablet = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad || getenv("NESTVANE_TEST_TABLET");
    const auto row = [](NSString* symbol, NSString* title, NSString* detail, NSString* url) {
        return @{@"symbol": symbol, @"title": title, @"detail": detail ?: @"", @"url": url ?: @""};
    };
    _sections = @[
        @{@"title": @"Android", @"footer": [(fast
              ? @"120 Hz makes motion smoother and uses more battery. It takes effect the next time Android starts. Files you put into the Android folder appear in Android within a few seconds."
              : @"Files you put into the Android folder appear in Android within a few seconds.")
              stringByAppendingString:tablet ? @" Tablet layout makes Android lay its screens out as on a tablet, from the next time it starts. It is new: turn it off here if Android does not show properly." : @""],
          @"rows": [@[row(@"cpu", @"Android version", @"17", nil), row(@"display", @"Display", rate, nil)]
              arrayByAddingObjectsFromArray:[[(fast ? @[@{@"symbol": @"gauge.with.dots.needle.67percent", @"title": @"120 Hz",
                                                         @"detail": @"", @"url": @"", @"switch": Display120HzKey}] : @[])
              arrayByAddingObjectsFromArray:(tablet ? @[@{@"symbol": @"rectangle.split.2x1", @"title": @"Tablet layout (experimental)",
                                                          @"detail": @"", @"url": @"", @"switch": TabletLayoutKey}] : @[])]
              arrayByAddingObject:row(@"folder", @"Shared files", @"Open in Files", files)]]},
        @{@"title": @"Root", @"footer": RootFooter(),
          @"rows": [@[@{@"symbol": @"lock.open", @"title": @"Allow root", @"detail": @"", @"url": @"", @"switch": RootEnabledKey}]
              arrayByAddingObjectsFromArray:RootGrantRows()]},
        @{@"title": @"Updates", @"footer": @"Once a day Nestvane asks GitHub which version is the latest. It does not install anything itself.",
          @"rows": [(AvailableUpdate() ? @[row(@"arrow.down.circle", [NSString stringWithFormat:@"Version %@ available", AvailableUpdate()],
                                               @"See the release", UpdateReleaseAddress)] : @[])
              arrayByAddingObject:@{@"symbol": @"arrow.triangle.2.circlepath", @"title": @"Check for updates",
                                    @"detail": @"", @"url": @"", @"switch": UpdateCheckKey}]},
        @{@"title": @"Community", @"footer": @"",
          @"rows": @[row(@"globe", @"Website", @"atomsprout.com/nestvane", @"https://atomsprout.com/nestvane/"),
                     row(@"bubble.left.and.bubble.right", @"Discord", @"Join the server", @"https://discord.gg/eGzEaP6TzR"),
                     row(@"paperplane", @"Telegram", @"t.me/atomsprout", @"https://t.me/atomsprout"),
                     row(@"envelope", @"Email support", @"atomsprout@gmail.com", @"mailto:atomsprout@gmail.com?subject=Nestvane%20support")]},
        @{@"title": @"About", @"footer": @"Android is a trademark of Google LLC. Nestvane is not affiliated with or endorsed by Google.",
          @"rows": @[row(@"hand.raised", @"Privacy policy", nil, @"https://atomsprout.com/nestvane/#privacy")]},
    ];
    UIImageView* icon = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"Emblem"]];
    icon.layer.cornerRadius = 20;
    icon.layer.cornerCurve = kCACornerCurveContinuous;
    icon.clipsToBounds = YES;
    icon.isAccessibilityElement = NO;
    UILabel* name = [UILabel new];
    name.text = @"Nestvane";
    name.font = [UIFont systemFontOfSize:28 weight:UIFontWeightBold];
    name.adjustsFontForContentSizeCategory = YES;
    UILabel* version = [UILabel new];
    NSDictionary* info = NSBundle.mainBundle.infoDictionary;
    version.text = [NSString stringWithFormat:@"Version %@ (%@)", info[@"CFBundleShortVersionString"], info[@"CFBundleVersion"]];
    version.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    version.textColor = UIColor.secondaryLabelColor;
    version.adjustsFontForContentSizeCategory = YES;
    UILabel* line = [UILabel new];
    line.text = @"Android, running inside an app on your iPhone.";
    line.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    line.textColor = UIColor.secondaryLabelColor;
    line.textAlignment = NSTextAlignmentCenter;
    line.numberOfLines = 0;
    line.adjustsFontForContentSizeCategory = YES;
    UIStackView* header = [[UIStackView alloc] initWithArrangedSubviews:@[icon, name, version, line]];
    header.axis = UILayoutConstraintAxisVertical;
    header.alignment = UIStackViewAlignmentCenter;
    header.spacing = 6;
    [header setCustomSpacing:14 afterView:icon];
    header.layoutMargins = UIEdgeInsetsMake(24, 24, 12, 24);
    header.layoutMarginsRelativeArrangement = YES;
    [NSLayoutConstraint activateConstraints:@[[icon.widthAnchor constraintEqualToConstant:88], [icon.heightAnchor constraintEqualToConstant:88]]];
    header.frame = CGRectMake(0, 0, 320, [header systemLayoutSizeFittingSize:CGSizeMake(320, 0)
        withHorizontalFittingPriority:UILayoutPriorityRequired verticalFittingPriority:UILayoutPriorityFittingSizeLevel].height);
    self.tableView.tableHeaderView = header;
}

- (void)close {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView { return NSInteger(_sections.count); }
- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return NSInteger([_sections[section][@"rows"] count]);
}
- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section { return _sections[section][@"title"]; }
- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
    NSString* footer = _sections[section][@"footer"];
    return footer.length ? footer : nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:@"row"]
        ?: [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:@"row"];
    NSDictionary* row = _sections[indexPath.section][@"rows"][indexPath.row];
    UIListContentConfiguration* content = UIListContentConfiguration.valueCellConfiguration;
    content.image = [UIImage systemImageNamed:row[@"symbol"]];
    content.text = row[@"title"];
    content.secondaryText = row[@"detail"];
    cell.contentConfiguration = content;
    const BOOL opens = [row[@"url"] length] > 0;
    cell.selectionStyle = opens ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    cell.accessoryView = opens ? [[UIImageView alloc] initWithImage:
        [UIImage systemImageNamed:@"arrow.up.right" withConfiguration:
            [UIImageSymbolConfiguration configurationWithPointSize:13 weight:UIImageSymbolWeightSemibold]]] : nil;
    cell.accessoryView.tintColor = UIColor.tertiaryLabelColor;
    cell.accessibilityTraits = opens ? UIAccessibilityTraitLink : UIAccessibilityTraitStaticText;
    if (NSString* key = row[@"switch"]) {
        UISwitch* toggle = [UISwitch new];
        toggle.on = [NSUserDefaults.standardUserDefaults boolForKey:key];
        toggle.accessibilityLabel = row[@"title"];
        [toggle addAction:[UIAction actionWithHandler:^(UIAction* action) {
            [NSUserDefaults.standardUserDefaults setBool:static_cast<UISwitch*>(action.sender).on forKey:key];
        }] forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
    }
    return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSString* address = _sections[indexPath.section][@"rows"][indexPath.row][@"url"];
    if (NSURL* url = address.length ? [NSURL URLWithString:address] : nil)
        [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
}
@end

@interface ValidationViewController : UIViewController <UIDocumentPickerDelegate>
- (void)installApkAtURL:(NSURL*)source;
- (void)chooseApkWhenReady;
- (void)composeText;
- (void)collectDiagnostics:(void (^)(NSURL* report))done;
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* details;
@property(nonatomic, strong) GuestDisplayView* display;
@property(nonatomic, strong) UIButton* run;
@property(nonatomic, strong) UIButton* importApk;
@property(nonatomic, strong) UIButton* more;
@property(nonatomic, strong) UIButton* back;
@property(nonatomic, strong) UIButton* home;
@property(nonatomic, strong) UIButton* recents;
@property(nonatomic, strong) UIStackView* startup;
@property(nonatomic, strong) EmblemView* emblem;
@property(nonatomic, strong) UILabel* apkStatus;
@end

@implementation ValidationViewController {
    std::shared_ptr<nestvane::GuestApkInbox> _apkInbox;
    NSURL* _pendingApk;   // handed over before Android was started
    BOOL _pendingPicker;  // the quick action came before Android was started
    std::stop_source _androidStop;
    BOOL _started;
    NSUInteger _apkStatusGeneration;
    bool _displayMadeWide;  // the shape Android's display was given when it started
    int _rotationAsked;     // the quarter turns last asked of Android
    bool _rotationHeld;     // a test asked for a rotation: the view's shape no longer decides
    void (^_afterStop)(void);  // what waits for Android to have stopped
    NSURL* _backupFile;        // a backup written, for the person to choose where it goes
    BOOL _backupBusy;          // a backup or a restore is under way
    BOOL _startAfterBackup;    // Android was running when the backup began
    enum { PickApk, PickBackupPlace, PickBackup } _picking;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

// Tells Android the most frames a second to draw for the heat the phone is in; 0 when there is no limit.
- (void)heatChanged {
    dispatch_async(dispatch_get_main_queue(), ^{
        const int rate = int(GuestDisplayRate()), most = RateInHeat(rate);
        if (self->_apkInbox) self->_apkInbox->submitDisplayRate(most < rate ? most : 0);
    });
}

// The device was turned: Android is asked to hold its display the same way, a quarter turn from how
// the display was made when the view has the other shape. The picture follows when Android has turned.
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    const CGSize size = self.display.bounds.size;
    if (!_apkInbox || _rotationHeld || self.display.hidden || size.width <= 0 || size.height <= 0) return;
    const int wanted = (size.width > size.height) != _displayMadeWide ? 1 : 0;
    if (wanted == _rotationAsked) return;
    if (_apkInbox->submitRotation(wanted)) _rotationAsked = wanted;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    self.display = [GuestDisplayView new];
    self.display.translatesAutoresizingMaskIntoConstraints = NO;
    self.display.userInteractionEnabled = YES;
    self.display.contentMode = UIViewContentModeScaleAspectFit;
    self.display.accessibilityLabel = @"Android display";
    self.display.isAccessibilityElement = YES;
    self.display.hidden = YES;
    [self.view addSubview:self.display];

    self.back = ToolbarButton(@"chevron.backward", @"Android Back", self, @selector(guestBack));
    self.home = ToolbarButton(@"house", @"Android Home", self, @selector(guestHome));
    self.recents = ToolbarButton(@"square.on.square", @"Recent Apps", self, @selector(guestRecents));
    // Touch and hold Home: clear the apps in the background, as Clear all among Android's recent apps.
    __weak ValidationViewController* weakSelf = self;
    self.home.menu = [UIMenu menuWithChildren:@[
        [UIAction actionWithTitle:@"Clear Background Apps" image:[UIImage systemImageNamed:@"xmark.circle"] identifier:nil
                          handler:^(UIAction*) { [weakSelf clearBackgroundApps]; }]]];
    self.home.showsMenuAsPrimaryAction = NO;
    self.importApk = ToolbarButton(@"square.and.arrow.down", @"Install APK…", self, @selector(chooseApk));
    self.run = ToolbarButton(@"play.fill", @"Start Android", self, @selector(runOrStop));
    // Back, Home and Recent Apps are used all the time and sit alone in the bar. What is used now and then, and
    // what ends the session, is one step away in a menu: stopping Android also asks first. The
    // install button is no longer in the bar; it still says whether an installation may start.
    self.more = ToolbarButton(@"ellipsis.circle", @"More", nil, nil);
    self.more.showsMenuAsPrimaryAction = YES;
    __weak ValidationViewController* menuOwner = self;
    self.more.menu = [UIMenu menuWithChildren:@[[UIDeferredMenuElement elementWithUncachedProvider:
        ^(void (^completion)(NSArray<UIMenuElement*>*)) {
            ValidationViewController* owner = menuOwner;
            if (!owner) return completion(@[]);
            const bool running = owner->_androidStop.stop_possible() && !owner->_androidStop.stop_requested();
            UIAction* install = [UIAction actionWithTitle:@"Install APK…"
                                                    image:[UIImage systemImageNamed:@"square.and.arrow.down"]
                                               identifier:nil handler:^(UIAction*) { [owner chooseApk]; }];
            if (!owner->_apkInbox || !owner.importApk.enabled) install.attributes = UIMenuElementAttributesDisabled;
            UIAction* power = running
                ? [UIAction actionWithTitle:@"Stop Android…" image:[UIImage systemImageNamed:@"power"]
                                 identifier:nil handler:^(UIAction*) { [owner confirmStop]; }]
                : [UIAction actionWithTitle:@"Start Android" image:[UIImage systemImageNamed:@"play.fill"]
                                 identifier:nil handler:^(UIAction*) { if (owner.run.enabled) [owner runOrStop]; }];
            if (running) power.attributes = UIMenuElementAttributesDestructive;
            else if (!owner.run.enabled) power.attributes = UIMenuElementAttributesDisabled;
            UIAction* settings = [UIAction actionWithTitle:@"Settings" image:[UIImage systemImageNamed:@"gearshape"]
                                                identifier:nil handler:^(UIAction*) {
                UINavigationController* sheet = [[UINavigationController alloc] initWithRootViewController:[SettingsViewController new]];
                sheet.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
                [owner presentViewController:sheet animated:YES completion:nil];
            }];
            if (NSString* version = AvailableUpdate()) settings.subtitle = [NSString stringWithFormat:@"Version %@ is available", version];
            // Android has no phone number and no radio: a text message is one the person writes here,
            // which Android then shows as received.
            UIAction* text = [UIAction actionWithTitle:@"Send Text Message…" image:[UIImage systemImageNamed:@"message"]
                                            identifier:nil handler:^(UIAction*) { [owner composeText]; }];
            if (!running || !owner->_apkInbox) text.attributes = UIMenuElementAttributesDisabled;
            UIAction* bug = [UIAction actionWithTitle:@"Submit a Bug…" image:[UIImage systemImageNamed:@"ladybug"]
                                           identifier:nil handler:^(UIAction*) {
                [UIApplication.sharedApplication openURL:BugReportAddress(running) options:@{} completionHandler:nil];
            }];
            UIAction* diagnostics = [UIAction actionWithTitle:@"Export Diagnostics…"
                                                        image:[UIImage systemImageNamed:@"stethoscope"]
                                                   identifier:nil handler:^(UIAction*) { [owner exportDiagnostics]; }];
            UIAction* backup = [UIAction actionWithTitle:@"Back Up Android…" image:[UIImage systemImageNamed:@"externaldrive"]
                                              identifier:nil handler:^(UIAction*) { [owner confirmBackup]; }];
            UIAction* restore = [UIAction actionWithTitle:@"Restore Android…" image:[UIImage systemImageNamed:@"clock.arrow.circlepath"]
                                               identifier:nil handler:^(UIAction*) { [owner confirmRestore]; }];
            if (owner->_backupBusy || (owner->_androidStop.stop_possible() && !running))
                backup.attributes = restore.attributes = UIMenuElementAttributesDisabled;
            UIMenu* backups = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline
                                           children:@[backup, restore]];
            completion(@[install, text, settings, backups, bug, diagnostics, power]);
        }]]];
    UIStackView* controls = [[UIStackView alloc] initWithArrangedSubviews:
        @[self.back, self.home, self.recents, self.more, self.run]];
    controls.distribution = UIStackViewDistributionFillEqually;
    controls.translatesAutoresizingMaskIntoConstraints = NO;

    UIView* footer = [UIView new];
    footer.backgroundColor = [UIColor colorWithWhite:0.10 alpha:1];
    footer.translatesAutoresizingMaskIntoConstraints = NO;
    [footer addSubview:controls];
    [self.view addSubview:footer];

    self.status = [UILabel new];
    self.status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    self.status.adjustsFontForContentSizeCategory = YES;
    self.status.textColor = UIColor.whiteColor;
    self.status.textAlignment = NSTextAlignmentCenter;
    self.status.numberOfLines = 0;

    self.details = [UITextView new];
    self.details.editable = NO;
    self.details.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    self.details.backgroundColor = UIColor.secondarySystemBackgroundColor;
    self.details.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);
    self.details.hidden = YES;

    // NESTVANE_TEST_SCREEN=start or =settings saves what the app itself draws, which the frames of
    // Android's display do not show: the start screen, or the settings sheet, as app-screen.png.
    // NESTVANE_TEST_SCREEN=bug writes the address Submit a Bug would open to bug-report-url.txt, and opens nothing.
    if ([NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_SCREEN"] isEqualToString:@"bug"]) {
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        [BugReportAddress(false).absoluteString writeToURL:[documents URLByAppendingPathComponent:@"bug-report-url.txt"]
                                               atomically:YES encoding:NSUTF8StringEncoding error:nil];
    }
    // NESTVANE_TEST_TURN=<seconds> asks the system to turn the app to landscape that long after it
    // starts, as turning the device does; a second number, =<seconds>:<seconds>, turns it back then.
    if (NSString* turn = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_TURN"]) {
        __weak ValidationViewController* shown = self;
        NSArray<NSString*>* times = [turn componentsSeparatedByString:@":"];
        for (NSUInteger index = 0; index < times.count && index < 2; ++index) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(times[index].doubleValue * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                UIWindowScene* scene = shown.view.window.windowScene;
                const UIInterfaceOrientationMask mask = index ? UIInterfaceOrientationMaskPortrait : UIInterfaceOrientationMaskLandscapeRight;
                [scene requestGeometryUpdateWithPreferences:[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask]
                                               errorHandler:^(NSError* error) { NSLog(@"turn refused: %@", error); }];
            });
        }
    }
    // NESTVANE_TEST_ROTATE=<quarter turns>:<seconds> asks Android to turn its display that long after
    // the app starts, as viewDidLayoutSubviews does when the device is turned.
    if (NSString* rotate = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_ROTATE"]) {
        __weak ValidationViewController* shown = self;
        NSArray<NSString*>* parts = [rotate componentsSeparatedByString:@":"];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(parts.lastObject.doubleValue * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            ValidationViewController* owner = shown;
            if (owner && owner->_apkInbox && owner->_apkInbox->submitRotation(parts.firstObject.intValue))
                owner->_rotationHeld = true;
        });
    }
    // NESTVANE_TEST_SCREEN=window:<seconds> saves the whole window that long after the app starts: how
    // Android's display sits in it, turned or not.
    if (NSString* wanted = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_SCREEN"]) {
        __weak ValidationViewController* shown = self;
        const double after = [wanted hasPrefix:@"window:"] ? [wanted substringFromIndex:7].doubleValue : 2;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(after * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            ValidationViewController* owner = shown;
            UIWindow* window = owner.view.window;
            if (!window) return;
            const auto save = ^{
                UIImage* image = [[[UIGraphicsImageRenderer alloc] initWithBounds:window.bounds]
                    imageWithActions:^(UIGraphicsImageRendererContext*) { [window drawViewHierarchyInRect:window.bounds afterScreenUpdates:YES]; }];
                NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
                [UIImagePNGRepresentation(image) writeToURL:[documents URLByAppendingPathComponent:@"app-screen.png"] atomically:YES];
            };
            if (![wanted isEqualToString:@"settings"]) return save();
            UINavigationController* sheet = [[UINavigationController alloc] initWithRootViewController:[SettingsViewController new]];
            sheet.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
            sheet.modalPresentationStyle = UIModalPresentationFullScreen;
            [owner presentViewController:sheet animated:NO completion:^{
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), save);
            }];
        });
    }
    self.emblem = [EmblemView new];
    self.emblem.active = YES;
    self.startup = [[UIStackView alloc] initWithArrangedSubviews:@[self.emblem, self.status, self.details]];
    self.startup.axis = UILayoutConstraintAxisVertical;
    self.startup.spacing = 12;
    self.startup.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.startup];

    self.apkStatus = [UILabel new];
    self.apkStatus.translatesAutoresizingMaskIntoConstraints = NO;
    self.apkStatus.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    self.apkStatus.adjustsFontForContentSizeCategory = YES;
    self.apkStatus.textColor = UIColor.whiteColor;
    self.apkStatus.backgroundColor = [UIColor colorWithWhite:0.10 alpha:0.95];
    self.apkStatus.textAlignment = NSTextAlignmentCenter;
    self.apkStatus.numberOfLines = 0;
    self.apkStatus.hidden = YES;
    [self.view addSubview:self.apkStatus];

    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint* detailsHeight = [self.details.heightAnchor constraintEqualToConstant:260];
    detailsHeight.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [footer.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [footer.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [footer.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [controls.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [controls.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [controls.topAnchor constraintEqualToAnchor:footer.topAnchor],
        [controls.heightAnchor constraintEqualToConstant:44],
        [controls.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [self.display.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [self.display.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [self.display.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [self.display.bottomAnchor constraintEqualToAnchor:footer.topAnchor],
        [self.startup.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [self.startup.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
        [self.startup.centerYAnchor constraintEqualToAnchor:self.display.centerYAnchor],
        [self.startup.topAnchor constraintGreaterThanOrEqualToAnchor:self.display.topAnchor constant:8],
        [self.startup.bottomAnchor constraintLessThanOrEqualToAnchor:self.display.bottomAnchor constant:-8],
        detailsHeight,
        [self.apkStatus.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [self.apkStatus.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [self.apkStatus.bottomAnchor constraintEqualToAnchor:footer.topAnchor],
        [self.apkStatus.heightAnchor constraintGreaterThanOrEqualToConstant:30],
    ]];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (_started) return;
    _started = YES;
    __weak ValidationViewController* owner = self;
    // Said once for each new version, at the bottom for a few seconds; after that only the More menu's
    // Settings item and the top of Settings say it, until the person updates or turns the question off.
    AskForUpdate(^(NSString* version) {
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        if ([[defaults stringForKey:UpdateToldKey] isEqualToString:version]) return;
        [defaults setObject:version forKey:UpdateToldKey];
        [owner showApkStatus:[NSString stringWithFormat:@"Nestvane %@ is out. It’s in Settings when you want it.", version] transient:YES];
    });
    // NESTVANE_TEST_BACKUP=export writes a backup to backup-test.aar in Documents before Android
    // starts, and =import restores that file; what came of it is in backup-test.txt.
    if (NSString* test = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_BACKUP"]) {
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        NSURL* archive = [documents URLByAppendingPathComponent:@"backup-test.aar"];
        self.status.text = @"Testing backup…";
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            const auto started = NSProcessInfo.processInfo.systemUptime;
            NSString* error = nil;
            NSDictionary* settings = nil;
            if ([test isEqualToString:@"export"]) error = nestvane::backup::write(archive, AndroidSettings());
            else if ([test isEqualToString:@"import"]) error = nestvane::backup::restore(archive, AndroidSettings(), &settings);
            else error = @"unknown test";
            if (settings) ApplyAndroidSettings(settings);
            NSNumber* bytes = [archive resourceValuesForKeys:@[NSURLFileSizeKey] error:nil][NSURLFileSizeKey];
            [[NSString stringWithFormat:@"%@: %@ in %.1f s; archive %@ bytes, data %llu bytes, pending %d\n", test, error ?: @"done",
                NSProcessInfo.processInfo.systemUptime - started, bytes, nestvane::backup::dataBytes(),
                int(nestvane::backup::restorePending())]
                writeToURL:[documents URLByAppendingPathComponent:@"backup-test.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{ [self runValidation]; });
        });
        return;
    }
    [self runValidation];
}

- (void)setRunSymbol:(NSString*)symbol label:(NSString*)label {
    UIButtonConfiguration* configuration = self.run.configuration;
    configuration.image = [UIImage systemImageNamed:symbol];
    self.run.configuration = configuration;
    self.run.accessibilityLabel = label;
    // While Android runs there is no button in the bar that stops it: that is in the menu.
    self.run.hidden = [symbol isEqualToString:@"stop.fill"];
}

// Export Diagnostics, for when Android seems frozen: what the runtime and Android say of where they
// stand, with the records the app keeps, in one text file to attach to a bug report.
- (void)exportDiagnostics {
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Export Diagnostics?"
        message:@"Nestvane writes down what Android is doing now and what it logged last, for a bug report. "
                 "It takes a few seconds. The log can name your apps and accounts: read it before you share it."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Export" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
        [self showApkStatus:@"Collecting diagnostics…" transient:NO];
        [self collectDiagnostics:^(NSURL* report) {
            self.apkStatus.hidden = YES;
            UIActivityViewController* share = [[UIActivityViewController alloc] initWithActivityItems:@[report]
                                                                                applicationActivities:nil];
            share.popoverPresentationController.sourceView = self.more;
            [self presentViewController:share animated:YES completion:nil];
        }];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)collectDiagnostics:(void (^)(NSURL* report))done {
    const bool running = _androidStop.stop_possible() && !_androidStop.stop_requested();
    const auto finished = std::make_shared<std::atomic_bool>(false);
    const auto finish = ^(NSString* android) {
        if (finished->exchange(true)) return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            utsname system{};
            uname(&system);
            NSDictionary* info = NSBundle.mainBundle.infoDictionary;
            NSDateFormatter* format = [NSDateFormatter new];
            format.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
            format.dateFormat = @"yyyy-MM-dd HH:mm:ss ZZZZZ";
            NSMutableString* text = [NSMutableString stringWithFormat:
                @"Nestvane diagnostics, %@\nNestvane %@ (%@), iOS %@, %s, %.0f GB of memory, temperature %@, Android %@\n",
                [format stringFromDate:NSDate.date], info[@"CFBundleShortVersionString"], info[@"CFBundleVersion"],
                UIDevice.currentDevice.systemVersion, system.machine, std::round(NSProcessInfo.processInfo.physicalMemory / 1e9),
                ThermalState(), running ? @"running" : @"not running"];
            std::string memory;
            AppendMemoryUsage(memory);
            [text appendFormat:@"%s\n== Android and the runtime\n%@\n", memory.c_str(), android];
            // What the app wrote down as it went: how the last run ended (for an Android that never got
            // past its start screen, or stopped), the start, the first frame, the network, and the last
            // state a watchdog saw. These are from the last run even when Android is not running now.
            NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
            for (NSString* name in @[@"validation.txt", @"boot-milestones.txt", @"launcher-displayed.txt",
                                     @"display-first-frame.txt", @"network-path.txt", @"network-check.txt",
                                     @"guest-watchdog.txt", @"guest-live.txt", @"boot-progress.txt"]) {
                NSData* data = [NSData dataWithContentsOfURL:[documents URLByAppendingPathComponent:name]];
                if (!data.length) continue;
                // The start's log is long: its end is what matters.
                constexpr NSUInteger most = 128 * 1024;
                if (data.length > most) data = [data subdataWithRange:NSMakeRange(data.length - most, most)];
                NSString* body = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"(not text)";
                [text appendFormat:@"\n== %@\n%@\n", name, body];
            }
            [format setDateFormat:@"yyyyMMdd-HHmmss"];
            NSURL* file = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:
                [NSString stringWithFormat:@"Nestvane-Diagnostics-%@.txt", [format stringFromDate:NSDate.date]]];
            [text writeToURL:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{ done(file); });
        });
    };
    if (!running || !nestvane::requestDiagnostics([finish](std::string report) {
            finish([[NSString alloc] initWithBytes:report.data() length:report.size() encoding:NSUTF8StringEncoding]
                   ?: @"(the report was not text)");
        })) {
        finish(@"Android was not running.");
        return;
    }
    // A runtime that is itself stuck does not answer: then what was written down before is all there is.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        finish(@"Android did not answer within 15 seconds: the runtime itself is stuck. "
                "Below is only what the app wrote down before.");
    });
}

// Runs `then` once Android has stopped, stopping it first when it runs; says whether it was running.
- (void)whenAndroidStopped:(void (^)(BOOL wasRunning))then {
    if (!_androidStop.stop_possible()) return then(NO);
    const BOOL running = !_androidStop.stop_requested();
    _afterStop = ^{ then(running); };
    [self showApkStatus:@"Stopping Android…" transient:NO];
    if (running) [self runOrStop];
}

- (void)startAndroidIf:(BOOL)wanted {
    if (wanted && !_androidStop.stop_possible() && self.run.enabled) [self runValidation];
}

// Back Up Android: Android stops, its data goes into one file, and the person chooses where to keep it.
- (void)confirmBackup {
    const bool running = _androidStop.stop_possible() && !_androidStop.stop_requested();
    NSString* bytes = [NSByteCountFormatter stringFromByteCount:(long long)nestvane::backup::dataBytes()
                                                     countStyle:NSByteCountFormatterCountStyleFile];
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Back Up Android?"
        message:[NSString stringWithFormat:@"Android’s apps, their data and its settings are saved in one file of up to %@, "
                                           @"and you choose where to keep it. Files in the Android folder are not in it: "
                                           @"they are in the Files app already.%@", bytes,
                                           running ? @"\n\nAndroid stops while it is saved, and starts again afterwards." : @""]
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Back Up" style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction*) { [self backUp]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)backUp {
    if (_backupBusy) return;
    _backupBusy = YES;
    [self whenAndroidStopped:^(BOOL wasRunning) {
        [self showApkStatus:@"Backing up Android…" transient:NO];
        __block UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Backup" expirationHandler:^{
            [UIApplication.sharedApplication endBackgroundTask:task];
            task = UIBackgroundTaskInvalid;
        }];
        NSDictionary* settings = AndroidSettings();
        NSDateFormatter* format = [NSDateFormatter new];
        format.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        format.dateFormat = @"yyyy-MM-dd-HHmm";
        NSURL* file = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:
            [NSString stringWithFormat:@"Nestvane-Backup-%@.aar", [format stringFromDate:NSDate.date]]];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSString* error = nestvane::backup::write(file, settings);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
                self->_backupBusy = NO;
                if (error) {
                    [self showApkStatus:error transient:YES];
                    return [self startAndroidIf:wasRunning];
                }
                self->_backupFile = file;
                self->_startAfterBackup = wasRunning;
                // NESTVANE_TEST_BACKUP_SAVE=1 keeps it as backup-test.aar in Documents, as if chosen there.
                if (getenv("NESTVANE_TEST_BACKUP_SAVE")) {
                    NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
                    NSURL* kept = [documents URLByAppendingPathComponent:@"backup-test.aar"];
                    [NSFileManager.defaultManager removeItemAtURL:kept error:nil];
                    [NSFileManager.defaultManager moveItemAtURL:file toURL:kept error:nil];
                    return [self backupPlaced:YES];
                }
                self.apkStatus.hidden = YES;
                self->_picking = PickBackupPlace;
                UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[file] asCopy:NO];
                picker.delegate = self;
                [self presentViewController:picker animated:YES completion:nil];
            });
        });
    }];
}

- (void)backupPlaced:(BOOL)placed {
    if (_backupFile) [NSFileManager.defaultManager removeItemAtURL:_backupFile error:nil];
    _backupFile = nil;
    [self showApkStatus:placed ? @"Android is backed up." : @"The backup was not kept." transient:YES];
    [self startAndroidIf:std::exchange(_startAfterBackup, NO)];
}

// Restore Android: the person chooses a backup, Android stops, and starts again from the backup.
- (void)confirmRestore {
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Restore Android?"
        message:@"Android’s apps, their data and its settings are replaced by those in a backup you choose, and Android starts "
                @"from it. What Android has now is kept until then, and put back if Android does not start."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Choose Backup…" style:UIAlertActionStyleDestructive handler:^(UIAlertAction*) {
        self->_picking = PickBackup;
        UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc]
            initForOpeningContentTypes:@[UTTypeData] asCopy:NO];  // read where it is: a backup is large
        picker.delegate = self;
        [self presentViewController:picker animated:YES completion:nil];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)restoreFrom:(NSURL*)source {
    if (_backupBusy) return;
    _backupBusy = YES;
    const BOOL scoped = [source startAccessingSecurityScopedResource];
    [self whenAndroidStopped:^(BOOL) {
        [self showApkStatus:@"Restoring Android…" transient:NO];
        __block UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Restore" expirationHandler:^{
            [UIApplication.sharedApplication endBackgroundTask:task];
            task = UIBackgroundTaskInvalid;
        }];
        NSDictionary* current = AndroidSettings();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSDictionary* settings = nil;
            NSString* error = nestvane::backup::restore(source, current, &settings);
            if (scoped) [source stopAccessingSecurityScopedResource];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
                self->_backupBusy = NO;
                if (!error) ApplyAndroidSettings(settings);
                [self startAndroidIf:YES];
                [self showApkStatus:error ?: @"Android is starting from the backup." transient:YES];
            });
        });
    }];
}

- (void)confirmStop {
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Stop Android?"
                                                                   message:@"Apps that are running in Android will be closed."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Stop" style:UIAlertActionStyleDestructive handler:^(UIAlertAction*) {
        if (self->_androidStop.stop_possible()) [self runOrStop];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showApkStatus:(NSString*)message transient:(BOOL)transient {
    NSLog(@"APK: %@", message);
    const auto generation = ++_apkStatusGeneration;
    self.apkStatus.text = message;
    self.apkStatus.hidden = NO;
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, message);
    if (!transient) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (self->_apkStatusGeneration == generation) self.apkStatus.hidden = YES;
    });
}

- (void)runOrStop {
    if (_androidStop.stop_possible()) {
        _androidStop.request_stop();
        self.run.enabled = NO;
        self.importApk.enabled = NO;
    } else {
        [self runValidation];
    }
}

- (void)guestBack {
    [self.display pressKey:nestvane::GuestTouch::Key::back];
}

- (void)guestHome {
    [self.display pressKey:nestvane::GuestTouch::Key::home];
}

- (void)guestRecents {
    [self.display pressKey:nestvane::GuestTouch::Key::recents];
}

- (void)clearBackgroundApps {
    if (_apkInbox) _apkInbox->submitClean();
}

- (void)chooseApk {
    if (!_apkInbox || !self.importApk.enabled) return;
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[UTTypeData] asCopy:YES];  // not every provider tags .apk files with a type
    picker.delegate = self;
    _picking = PickApk;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    switch (std::exchange(_picking, PickApk)) {
        case PickBackupPlace: return [self backupPlaced:YES];
        case PickBackup: if (NSURL* source = urls.firstObject) [self restoreFrom:source]; return;
        case PickApk: if (NSURL* source = urls.firstObject) [self installApkAtURL:source]; return;
    }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    if (std::exchange(_picking, PickApk) == PickBackupPlace) [self backupPlaced:NO];
}

// The home screen's quick action: the picker, as soon as there is something to present it from.
- (void)chooseApkWhenReady {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.presentedViewController) return;
        if (!self->_apkInbox) self->_pendingPicker = YES;
        else [self chooseApk];
    });
}

// Asks for a sender and a text, and hands them to Android, which shows them as a message it received
// (fixtures/android17-text.sh). Android takes it once it has booted.
- (void)composeText {
    UIAlertController* sheet = [UIAlertController alertControllerWithTitle:@"Send Text Message"
        message:@"Android receives this as an incoming text. Nothing is sent to anyone."
        preferredStyle:UIAlertControllerStyleAlert];
    [sheet addTextFieldWithConfigurationHandler:^(UITextField* field) {
        field.placeholder = @"From";
        field.text = @"+1 555 0100";
        field.keyboardType = UIKeyboardTypePhonePad;
    }];
    [sheet addTextFieldWithConfigurationHandler:^(UITextField* field) { field.placeholder = @"Message"; }];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak UIAlertController* fields = sheet;
    __weak ValidationViewController* owner = self;
    [sheet addAction:[UIAlertAction actionWithTitle:@"Send" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
        ValidationViewController* controller = owner;
        NSString* sender = [fields.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSString* text = [fields.textFields[1].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!controller) return;
        const auto inbox = controller->_apkInbox;
        const bool taken = inbox && sender.length && text.length && inbox->submitText(sender.UTF8String, text.UTF8String);
        [controller showApkStatus:taken ? @"Text message handed to Android." : @"The text message needs a sender and a text."
                        transient:YES];
    }]];
    sheet.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    [self presentViewController:sheet animated:YES completion:nil];
}

// An APK is a ZIP archive with AndroidManifest.xml in it: a local file header first, and the name
// in the central directory at the end.
static bool ApkContent(NSData* data) {
    const auto* bytes = static_cast<const uint8_t*>(data.bytes);
    if (data.length < 64 || memcmp(bytes, "PK\x03\x04", 4) != 0) return false;
    static constexpr char name[] = "AndroidManifest.xml";
    const size_t tail = std::min<size_t>(data.length, 4 * 1024 * 1024);
    const auto* start = bytes + data.length - tail;
    return memmem(start, tail, name, sizeof(name) - 1) != nullptr ||
           memmem(bytes, std::min<size_t>(data.length, 1024 * 1024), name, sizeof(name) - 1) != nullptr;
}

// Installs an APK from the picker, or one that another app handed over ("Open in", AirDrop, a quick
// action). Android picks it up when it has booted, so this can come before the launcher is there.
- (void)installApkAtURL:(NSURL*)source {
    const auto inbox = _apkInbox;
    if (!inbox) {
        // Android is started when the view appears, and a file can arrive before that.
        _pendingApk = source;
        return;
    }
    if (!self.importApk.enabled) {
        [self showApkStatus:@"Another APK is still being installed." transient:YES];
        return;
    }
    // Judged by its content once read (ApkContent): another app may hand over an APK under a name
    // of its own, without the extension.
    self.importApk.enabled = NO;
    [self showApkStatus:@"Waiting for Android to install the APK…" transient:NO];
    __weak ValidationViewController* weakSelf = self;
    const std::weak_ptr<nestvane::GuestApkInbox> active = inbox;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        using Inbox = nestvane::GuestApkInbox;
        const auto report = [weakSelf, active](NSString* message) {
            dispatch_async(dispatch_get_main_queue(), ^{
                ValidationViewController* owner = weakSelf;
                const auto current = active.lock();
                if (!owner || !current || owner->_apkInbox != current) return;
                [owner showApkStatus:message transient:YES];
                owner.importApk.enabled = owner->_androidStop.stop_possible() && !owner->_androidStop.stop_requested();
            });
        };
        const BOOL access = [source startAccessingSecurityScopedResource];
        // Read first and judge by what was read: a file another app hands over does not always
        // answer questions about its attributes.
        NSError* readError = nil;
        NSData* data = [NSData dataWithContentsOfURL:source options:NSDataReadingMappedIfSafe error:&readError];
        if (!data.length) {
            NSLog(@"APK: unreadable: %@", readError);
            if (access) [source stopAccessingSecurityScopedResource];
            report(@"Couldn’t read the selected APK.");
            return;
        }
        if (data.length > Inbox::maxBytes) {
            if (access) [source stopAccessingSecurityScopedResource];
            report(@"Choose an APK file up to 1 GB.");
            return;
        }
        if (!ApkContent(data)) {
            if (access) [source stopAccessingSecurityScopedResource];
            NSLog(@"APK: not an Android package: %@", source.lastPathComponent);
            report(@"This file isn’t an Android app (APK).");
            return;
        }
        // The bytes as read (mapped from the file when it can be), kept by the request: not copied.
        const std::span<const uint8_t> file(static_cast<const uint8_t*>(data.bytes), data.length);
        const std::shared_ptr<const void> owner((__bridge_retained const void*)data, [](const void* held) { CFRelease(held); });
        if (access) [source stopAccessingSecurityScopedResource];
        // A file another app handed over was copied into this app's inbox directory for it. Unlinked,
        // its mapped bytes stay readable until the installation is over.
        if ([source.path containsString:@"/Documents/Inbox/"]) [NSFileManager.defaultManager removeItemAtURL:source error:nil];
        if (!inbox->submit(file, owner, [report](Inbox::Result result) {
            switch (result) {
                case Inbox::Result::installed: report(@"APK installed. Open it from the Android launcher."); break;
                case Inbox::Result::rejected: report(@"Android couldn’t install this APK."); break;
                case Inbox::Result::storageError: report(@"Android could not save the installation."); break;
                case Inbox::Result::guestStopped: report(@"Android stopped before finishing the installation."); break;
            }
        })) report(@"Android couldn’t start the installation.");
    });
}

- (void)runValidation {
    const auto validationStarted = NSProcessInfo.processInfo.systemUptime;
    self.run.enabled = NO;
    // The app declares iOS 17 as its minimum, and iOS itself refuses to install it on anything older.
    // A sideloading tool can take that declaration out; then the app says so here and starts nothing,
    // since it has never been run on an older system. NESTVANE_TEST_OLD_SYSTEM=1 shows the message,
    // and NESTVANE_ALLOW_OLD_SYSTEM=1 starts Android all the same, for trying an older system.
    if ((NSProcessInfo.processInfo.operatingSystemVersion.majorVersion < 17 && !getenv("NESTVANE_ALLOW_OLD_SYSTEM")) ||
        getenv("NESTVANE_TEST_OLD_SYSTEM")) {
        self.status.text = [NSString stringWithFormat:@"Nestvane needs iOS 17 or later.\nThis iPhone has iOS %@.",
            UIDevice.currentDevice.systemVersion];
        self.status.numberOfLines = 0;
        self.status.textAlignment = NSTextAlignmentCenter;
        return;
    }
    NSString* installer = [NSBundle.mainBundle.resourcePath
        stringByAppendingPathComponent:@"android17-init-root/system/bin/nestvane-apk-install"];
    _apkInbox = [NSFileManager.defaultManager fileExistsAtPath:installer] ?
        std::make_shared<nestvane::GuestApkInbox>() : nullptr;
    RunningInbox = _apkInbox;
    const auto apkInbox = _apkInbox;
    static dispatch_once_t heatWatched;
    dispatch_once(&heatWatched, ^{
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(heatChanged)
            name:NSProcessInfoThermalStateDidChangeNotification object:nil];
        NSArray<NSString*>* pretend = [NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_HOT"] componentsSeparatedByString:@":"];
        NSArray<NSString*>* states = @[@"nominal", @"fair", @"serious", @"critical"];
        if (pretend.count == 2 && [states containsObject:pretend[0]])
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, pretend[1].intValue * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                HeatPretended = int([states indexOfObject:pretend[0]]);
                [NSNotificationCenter.defaultCenter postNotificationName:NSProcessInfoThermalStateDidChangeNotification object:nil];
            });
    });
    [self heatChanged];
    _androidStop = apkInbox ? std::stop_source{} : std::stop_source{std::nostopstate};
    const auto stop = _androidStop.get_token();
    if (stop.stop_possible()) {
        [self setRunSymbol:@"stop.fill" label:@"Stop Android"];
        self.run.enabled = YES;
    }
    self.importApk.hidden = !apkInbox;
    self.importApk.enabled = bool(apkInbox);
    if (apkInbox && (_pendingApk || _pendingPicker)) {
        NSURL* pending = std::exchange(_pendingApk, nil);
        const BOOL picker = std::exchange(_pendingPicker, NO);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (pending) [self installApkAtURL:pending];
            else if (picker) [self chooseApk];
        });
    }
    self.apkStatus.hidden = YES;
    auto touch = std::make_shared<nestvane::GuestTouch>();
    [self.display setTouchDevice:touch];
    [self.display show:nil size:CGSizeZero];
    self.display.hidden = YES;
    self.back.hidden = self.home.hidden = self.recents.hidden = !apkInbox;
    self.back.enabled = self.home.enabled = self.recents.enabled = NO;
    self.startup.hidden = NO;
    self.emblem.active = YES;
    self.details.hidden = YES;
    self.details.text = nil;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    self.status.text = apkInbox ? @"Starting Android…" : @"Running ARM64 ELF validation…";
    const auto displaySize = GuestDisplaySize(self.display.bounds.size);
    _displayMadeWide = displaySize.width > displaySize.height;
    _rotationAsked = -1;  // Android keeps the rotation it was last asked for: say again which it is to be
    // A thread of its own, not a dispatch queue's: the runtime ends guest time slices with a signal to
    // this thread, and the system refuses to send one to a queue's worker thread.
    NSThread* guestThread = [[NSThread alloc] initWithBlock:^{
        NSString* before = ThermalState();
        NSString* resources = NSBundle.mainBundle.resourcePath;
#if defined(NESTVANE_PAGE_SIZE) && NESTVANE_PAGE_SIZE != 4096
        // The validation fixtures are linked for 4 KiB guest pages, which this build does not use.
        nestvane::Validation result{true, "ARM64 ELF validation fixtures skipped: guest pages are not 4 KiB\n"};
#else
        auto result = nestvane::validate(resources.fileSystemRepresentation);
#endif
        NSString* status = result.passed ? @"Execution paths agree" : @"Validation failed";
        bool previousChecksPassed = result.passed;
#if defined(NESTVANE_PAGE_SIZE) && NESTVANE_PAGE_SIZE != 4096
        const bool fixturesFit = false;  // the dynamic ELF fixture is linked for 4 KiB guest pages too
#else
        const bool fixturesFit = true;
#endif
        if (previousChecksPassed && fixturesFit && [NSFileManager.defaultManager fileExistsAtPath:
                               [resources stringByAppendingPathComponent:@"android17-dynamic.elf"]]) {
            auto probe = nestvane::probeDynamic(resources.fileSystemRepresentation);
            result.report += "\n" + probe.report;
            status = probe.passed ? @"Android 17 dynamic ELF passed" : @"Android 17 loader probe stopped";
            previousChecksPassed = probe.passed;
        }
        NSString* platform =
#if TARGET_OS_SIMULATOR
            @"iOS Simulator";
#else
            @"iOS device";
#endif
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                               inDomains:NSUserDomainMask].firstObject;
        // Where this directory is on the device, for a test that hands the app a file by its URL.
        [documents.path writeToURL:[documents URLByAppendingPathComponent:@"documents-path.txt"] atomically:YES
                          encoding:NSUTF8StringEncoding error:nil];
        NSURL* outputURL = [documents URLByAppendingPathComponent:@"validation.txt"];
        const auto exportReport = [&](NSURL* destination, const std::string& output) {
            NSString* report = [NSString stringWithFormat:@"%@ %@ | process %d | thermal %@ → %@\n%@",
                platform, UIDevice.currentDevice.systemVersion, NSProcessInfo.processInfo.processIdentifier,
                before, ThermalState(), [NSString stringWithUTF8String:output.c_str()]];
            NSError* error = nil;
            if (![report writeToURL:destination atomically:YES encoding:NSUTF8StringEncoding error:&error])
                NSLog(@"Unable to export report: %@", error);
            return report;
        };
        NSString* artStatus = nil;
        bool failedBeforeHome = false;  // Android stopped by itself before it showed its home screen
        NSString* artRoot = [resources stringByAppendingPathComponent:@"android17-art-root"];
        if (previousChecksPassed && [NSFileManager.defaultManager fileExistsAtPath:artRoot]) {
            exportReport(outputURL, result.report + "\nAndroid 17 ART probe starting\n");
            auto probe = nestvane::probeArt(artRoot.fileSystemRepresentation);
            result.report += "\n" + probe.report;
            artStatus = probe.passed ? @"Android 17 ART executed DEX" : @"Android 17 ART probe stopped";
            exportReport(outputURL, result.report);
        }
        NSString* initRoot = [resources stringByAppendingPathComponent:@"android17-init-root"];
        if (previousChecksPassed && [NSFileManager.defaultManager fileExistsAtPath:initRoot]) {
            auto* manager = NSFileManager.defaultManager;
            NSURL* support = [manager URLsForDirectory:NSApplicationSupportDirectory
                                               inDomains:NSUserDomainMask].firstObject;
            NSURL* guest = [support URLByAppendingPathComponent:@"Nestvane" isDirectory:YES];
            NSError* directoryError = nil;
            const BOOL ready = [manager createDirectoryAtURL:guest withIntermediateDirectories:YES
                                                  attributes:nil error:&directoryError];
            NSURL* database = [guest URLByAppendingPathComponent:@"guest-data.sqlite3"];
            // Android's shared storage is a folder the Files app shows: On My iPhone > Nestvane > Android.
            setenv("NESTVANE_SHARED_STORAGE", [documents URLByAppendingPathComponent:@"Android"].fileSystemRepresentation, 1);
            // One request through the system's own loader, to Apple's connectivity check: on phones where
            // each app has to be allowed onto the network, this is what makes the system ask. Android's
            // sockets alone do not, and are refused until the user has answered. The outcome is kept for
            // finding out why Android has no network (network-check.txt).
            static dispatch_once_t networkAsked;
            dispatch_once(&networkAsked, ^{
                NSURL* checkURL = [documents URLByAppendingPathComponent:@"network-check.txt"];
                NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@"https://captive.apple.com/hotspot-detect.html"]
                                                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:15];
                request.HTTPMethod = @"HEAD";
                [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData*, NSURLResponse* response, NSError* error) {
                    NSString* outcome = error ? [NSString stringWithFormat:@"error %ld %@: %@\n", long(error.code), error.domain, error.localizedDescription]
                        : [NSString stringWithFormat:@"status %ld\n", long(static_cast<NSHTTPURLResponse*>(response).statusCode)];
                    [outcome writeToURL:checkURL atomically:YES encoding:NSUTF8StringEncoding error:nil];
                }] resume];
            });
            // The rate Android composes at is fixed when it starts (GuestDisplayRate).
            setenv("NESTVANE_DISPLAY_HZ", @(GuestDisplayRate()).stringValue.UTF8String, 1);
            // On an iPad the display has several times a phone's pixels. Android is told they are as
            // many times denser, so that its screens are laid out as on a phone and drawn sharp.
            // With the tablet layout chosen in the settings, the short side is 800 of Android's units
            // instead of 590, which is a tablet to Android: a taskbar, and two panes where apps have
            // them. It is off by default: before 1.16 the launcher stopped as it started at such a
            // density (fixed in the image), and apps' windows were not drawn on an iPad on 2026-11-23
            // (see results/G0.md), which has not been tried on an iPad since.
            if (!getenv("NESTVANE_DISPLAY_DENSITY") && std::min(displaySize.width, displaySize.height) > defaultGuestWidth) {
                const double shortSide = [NSUserDefaults.standardUserDefaults boolForKey:TabletLayoutKey] ? 800.0 : 590.0;
                const int density = int(std::lround(160.0 * std::min(displaySize.width, displaySize.height) / shortSide));
                setenv("NESTVANE_DISPLAY_DENSITY", @(density).stringValue.UTF8String, 1);
            }
            // Android's sound comes out of the phone.
            static dispatch_source_t audioWatch;
            static dispatch_once_t audioWatched;
            dispatch_once(&audioWatched, ^{
                audioWatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
                dispatch_source_set_timer(audioWatch, DISPATCH_TIME_NOW, NSEC_PER_SEC / 4, NSEC_PER_SEC / 20);
                dispatch_source_set_event_handler(audioWatch, ^{ FollowGuestMicrophone(); FollowGuestAudio(); });
                dispatch_resume(audioWatch);
            });
            // Android shows the phone's battery: its level, and whether it is charging.
            UIDevice.currentDevice.batteryMonitoringEnabled = YES;
            const auto reportBattery = ^{
                auto& battery = nestvane::GuestBattery::host();
                const float level = UIDevice.currentDevice.batteryLevel;
                battery.level = level < 0 ? -1 : int(std::lround(level * 100));
                switch (UIDevice.currentDevice.batteryState) {
                    case UIDeviceBatteryStateUnplugged: battery.state = nestvane::GuestBattery::unplugged; break;
                    case UIDeviceBatteryStateCharging: battery.state = nestvane::GuestBattery::charging; break;
                    case UIDeviceBatteryStateFull: battery.state = nestvane::GuestBattery::full; break;
                    default: battery.state = nestvane::GuestBattery::unknown; break;
                }
            };
            reportBattery();
            for (NSNotificationName name in @[UIDeviceBatteryLevelDidChangeNotification, UIDeviceBatteryStateDidChangeNotification])
                [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
                                                             usingBlock:^(NSNotification*) { reportBattery(); }];
            // What the phone's network is, each time it changes, in network-path.txt with the time since
            // the start: for telling what Android did when the phone went from Wi-Fi to cellular or back.
            static nw_path_monitor_t pathMonitor;
            static NSMutableString* paths;
            if (pathMonitor) nw_path_monitor_cancel(pathMonitor);
            paths = [NSMutableString string];
            pathMonitor = nw_path_monitor_create();
            nw_path_monitor_set_queue(pathMonitor, dispatch_get_main_queue());
            NSURL* pathURL = [documents URLByAppendingPathComponent:@"network-path.txt"];
            nw_path_monitor_set_update_handler(pathMonitor, ^(nw_path_t path) {
                nestvane::hostnet::networkChanged();
                // Which network carries the traffic. When that changes, the connections made over the
                // old one are ended (HostSocket.hpp); iOS reports a path many times without a change.
                const int carrier = nw_path_get_status(path) != nw_path_status_satisfied ? 0
                    : nw_path_uses_interface_type(path, nw_interface_type_wifi) ? 1
                    : nw_path_uses_interface_type(path, nw_interface_type_wired) ? 2
                    : nw_path_uses_interface_type(path, nw_interface_type_cellular) ? 3 : 4;
                static int carrierBefore = -1;
                const bool moved = carrierBefore >= 0 && carrier != carrierBefore;
                carrierBefore = carrier;
                const size_t ended = moved ? nestvane::hostnet::endStreams() : 0;
                std::array<uint8_t, 12> gateway{};
                if (moved) [paths appendFormat:@"another network: ended %zu connections\n", ended];
                [paths appendFormat:@"%.0f ms | %s |%s%s%s%s%s\n", (NSProcessInfo.processInfo.systemUptime - validationStarted) * 1000,
                    nw_path_get_status(path) == nw_path_status_satisfied ? "connected" : "not connected",
                    nw_path_uses_interface_type(path, nw_interface_type_wifi) ? " Wi-Fi" : "",
                    nw_path_uses_interface_type(path, nw_interface_type_cellular) ? " cellular" : "",
                    nw_path_uses_interface_type(path, nw_interface_type_wired) ? " wired" : "",
                    nw_path_is_expensive(path) ? " (expensive)" : "",
                    nestvane::hostnet::nat64Prefix(gateway) ? " | no IPv4, through a gateway" : " | IPv4"];
                [paths writeToURL:pathURL atomically:YES encoding:NSUTF8StringEncoding error:nil];
            });
            nw_path_monitor_start(pathMonitor);
            // Android shows the time of the phone's time zone.
            setenv("NESTVANE_TIMEZONE", NSTimeZone.localTimeZone.name.UTF8String, 1);
            NSURL* progressURL = [documents URLByAppendingPathComponent:@"boot-progress.txt"];
            const auto bootStarted = NSProcessInfo.processInfo.systemUptime;
            std::string milestones, timeline;
            NSURL* timelineURL = [documents URLByAppendingPathComponent:@"boot-milestones.txt"];
            static constexpr std::string_view phasePrefixes[] = {
                "Initializing local Android renderer", "Local Android renderer initialized",
                "Starting Android 17 init", "Zygote: Preloading classes", "Zygote: end preload",
                "SystemServerTiming: StartPackageManagerService", "SystemServerTiming: StartSystemUI",
                "SystemServerTiming: broadcastIntent-0-android.intent.action.USER_UNLOCKED took to complete:",
                "SystemServerTiming: broadcastIntent-0-android.intent.action.BOOT_COMPLETED took to complete:",
                "ActivityTaskManager: Displayed com.android.launcher3/",
            };
            bool reportedPhase[std::size(phasePrefixes)]{};
            NSTimeInterval lastProgressExport = 0;
            std::atomic_bool launcherDisplayed{false};
            NSURL* launcherURL = [documents URLByAppendingPathComponent:@"launcher-displayed.txt"];
            const auto progress = [&](std::string_view event) {
                const auto now = NSProcessInfo.processInfo.systemUptime;
                const auto elapsed = static_cast<long long>((now - validationStarted) * 1000);
                milestones += "[" + std::to_string(static_cast<long long>((now - bootStarted) * 1000)) + " ms] ";
                milestones.append(event);
                milestones += '\n';
                static const size_t maxMilestoneBytes = (getenv("NESTVANE_LOG_ALL") ? 16 : 1) * 256 * 1024;  // the whole boot, for reading stage and service times afterwards
                if (milestones.size() > maxMilestoneBytes)
                    milestones.erase(0, milestones.find('\n', milestones.size() - maxMilestoneBytes) + 1);
                // The guest logs thousands of lines while it boots, and each call here is on its time:
                // the file is rewritten a few times a second, and whenever a boot phase is reached.
                const bool phase = std::any_of(std::begin(phasePrefixes), std::end(phasePrefixes),
                                               [&](std::string_view prefix) { return event.starts_with(prefix); });
                if (phase || now - lastProgressExport >= 0.25) {
                    lastProgressExport = now;
                    exportReport(progressURL, milestones);
                }
                for (size_t i = 0; i < std::size(phasePrefixes); ++i) {
                    if (reportedPhase[i] || !event.starts_with(phasePrefixes[i])) continue;
                    reportedPhase[i] = true;
                    timeline += "[" + std::to_string(elapsed) + " ms] ";
                    timeline.append(event.substr(0, event.find('\n')));
                    timeline += '\n';
                    exportReport(timelineURL, timeline);
                    break;
                }
                if (event.starts_with("ActivityTaskManager: Displayed com.android.launcher3/") &&
                    !launcherDisplayed.exchange(true, std::memory_order_relaxed)) {
                    std::string report = "Android Launcher displayed after " +
                        std::to_string(elapsed) + " ms\n";
                    report += event;
                    report += '\n';
                    AppendMemoryUsage(report);
                    exportReport(launcherURL, report);
                    // Android started from a restored backup: what it had before can go.
                    if (nestvane::backup::restorePending())
                        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ nestvane::backup::restoreProven(); });
                    // NESTVANE_ADDRESS_MAP=1: what fills the host address space, ten seconds on (PLAN.md, "先量 2.0").
                    if ([NSProcessInfo.processInfo.environment[@"NESTVANE_ADDRESS_MAP"] isEqualToString:@"1"]) {
                        NSURL* mapURL = [documents URLByAppendingPathComponent:@"address-map.txt"];
                        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                            std::string map;
                            AppendMemoryUsage(map);
                            AppendAddressMap(map);
                            NSData* data = [NSData dataWithBytes:map.data() length:map.size()];
                            [data writeToURL:mapURL atomically:YES];
                        });
                    }
                    // A few seconds on, when the home screen has drawn: which code pages this start used.
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                        NSString* list = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject
                            stringByAppendingPathComponent:@"boot-code-pages"];
                        nestvane::prefetch::record(list.fileSystemRepresentation);
                        nestvane::prefetch::recordFiles([list stringByAppendingString:@"-files"].fileSystemRepresentation,
                                                        NSBundle.mainBundle.resourcePath.fileSystemRepresentation);
                    });
                    // NESTVANE_TEST_TEXT="<sender>|<text>" hands Android a text message, as the menu does,
                    // ten seconds after the launcher is displayed.
                    if (NSString* message = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_TEXT"]; message && apkInbox) {
                        // Several messages, separated by ";;", go in two seconds apart.
                        NSArray<NSString*>* messages = [message componentsSeparatedByString:@";;"];
                        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                            for (NSString* one in messages) {
                                NSArray<NSString*>* parts = [one componentsSeparatedByString:@"|"];
                                if (parts.count != 2) continue;
                                apkInbox->submitText(parts[0].UTF8String, parts[1].UTF8String);
                                sleep(2);
                            }
                        });
                    }
                    // NESTVANE_TEST_SWIPE=1 checks touch without a finger: some seconds after the launcher
                    // is displayed, a swipe up the middle of the screen goes in through the same device
                    // the view feeds, with the frame saved before it and after it.
                    if ([NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_SWIPE"] isEqualToString:@"1"]) {
                        const auto device = touch;
                        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC),
                                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                            const auto save = ^(NSString* name) {
                                dispatch_sync(dispatch_get_main_queue(), ^{
                                    if (UIImage* frame = self.display.image)
                                        WriteGuestDisplayPNG(UIImagePNGRepresentation(frame),
                                                             [documents URLByAppendingPathComponent:name]);
                                });
                            };
                            // NESTVANE_TEST_APK=<file in Documents> installs that APK first, the way the
                            // Install APK button does, and writes the outcome to apk-result.txt.
                            // A list separated by commas installs one after the other.
                            // NESTVANE_TEST_APK_WAIT=<seconds> waits that long first: Android tells the
                            // launcher of a new package sooner once it has been up for a minute.
                            if (NSString* names = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_APK"]; names && apkInbox) {
                                sleep(unsigned(NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_APK_WAIT"].intValue));
                                NSMutableString* text = [NSMutableString string];
                                for (NSString* name in [names componentsSeparatedByString:@","]) @autoreleasepool {
                                    // Each APK's bytes go when it is installed, not when the whole test is over.
                                    NSData* data = [NSData dataWithContentsOfURL:[documents URLByAppendingPathComponent:name]
                                                                         options:NSDataReadingMappedIfSafe error:nil];
                                    const auto* bytes = static_cast<const uint8_t*>(data.bytes);
                                    const auto submitted = NSProcessInfo.processInfo.systemUptime;
                                    dispatch_semaphore_t done = dispatch_semaphore_create(0);
                                    const auto outcome = std::make_shared<std::atomic_int>(-1);
                                    const std::shared_ptr<const void> owner = data ?
                                        std::shared_ptr<const void>((__bridge_retained const void*)data, [](const void* held) { CFRelease(held); }) : nullptr;
                                    const bool accepted = data.length && apkInbox->submit(std::span<const uint8_t>(bytes, data.length), owner,
                                        [outcome, done](nestvane::GuestApkInbox::Result result) {
                                            outcome->store(int(result));
                                            dispatch_semaphore_signal(done);
                                        });
                                    if (accepted) dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC));
                                    [text appendFormat:@"%@ | %lu bytes | accepted %d | result %d (0 is installed) | %.1f s\n",
                                        name, (unsigned long)data.length, accepted, outcome->load(),
                                        NSProcessInfo.processInfo.systemUptime - submitted];
                                    [text writeToURL:[documents URLByAppendingPathComponent:@"apk-result.txt"] atomically:YES
                                            encoding:NSUTF8StringEncoding error:nil];
                                    // NESTVANE_TEST_APK_REMOVE=1 deletes the file once it is installed, so that a
                                    // batch of apps tried one after another leaves nothing in Documents.
                                    if ([NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_APK_REMOVE"] isEqualToString:@"1"])
                                        [NSFileManager.defaultManager removeItemAtURL:[documents URLByAppendingPathComponent:name] error:nil];
                                    sleep(1);
                                }
                                sleep(2);
                            }
                            save(@"touch-before.png");
                            const int32_t axis = nestvane::GuestTouch::maximumAxis;
                            for (int step = 0; step <= 20; ++step) {
                                device->submit(axis / 2, axis * (150 - step * 5) / 200, true);
                                usleep(15000);
                            }
                            device->submit(axis / 2, axis / 4, false);
                            sleep(3);
                            save(@"touch-after.png");
                            // NESTVANE_TEST_TAPS="x:y:hold:wait,..." then taps: position in thousandths of the
                            // screen, how long the finger stays down in milliseconds, and seconds to wait
                            // before the frame is saved as touch-tap-<n>.png.
                            NSString* taps = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_TAPS"];
                            int number = 0;
                            for (NSString* tap in [taps componentsSeparatedByString:@","]) {
                                NSArray<NSString*>* fields = [tap componentsSeparatedByString:@":"];
                                if (fields.count != 4) continue;
                                const int32_t x = axis * fields[0].intValue / 1000, y = axis * fields[1].intValue / 1000;
                                if ([fields[0] isEqualToString:@"launch"]) {
                                    // "launch:<package>:0:wait" opens that app without finding its icon.
                                    if (apkInbox) apkInbox->submitLaunch(fields[1].UTF8String);
                                } else if ([fields[0] hasPrefix:@"shell"]) {
                                    // "shell<n>:0:0:wait" runs NESTVANE_TEST_SHELL<n> in Android as its shell
                                    // user; the output goes to Android/nestvane-test/shell.txt in Documents.
                                    NSString* key = [@"NESTVANE_TEST_SHELL" stringByAppendingString:[fields[0] substringFromIndex:5]];
                                    if (NSString* command = NSProcessInfo.processInfo.environment[key]; command && apkInbox)
                                        apkInbox->submitLine("shell", command.UTF8String);
                                } else if ([fields[0] isEqualToString:@"clean"]) {
                                    // "clean:0:0:wait" clears the apps in the background, as Home's menu does.
                                    if (apkInbox) apkInbox->submitClean();
                                } else if ([fields[0] isEqualToString:@"uninstall"]) {
                                    // "uninstall:<package>:0:wait" removes that app.
                                    if (apkInbox) apkInbox->submitUninstall(fields[1].UTF8String);
                                } else if ([fields[0] hasPrefix:@"view"]) {
                                    // "view<x>:<y>:0:wait" taps a place in the app's view of the display, in
                                    // thousandths of the view: through the same reckoning as a finger's
                                    // touch, which the other taps go round.
                                    const int viewX = [fields[0] substringFromIndex:4].intValue, viewY = fields[1].intValue;
                                    dispatch_async(dispatch_get_main_queue(), ^{ [self.display tapAtX:viewX y:viewY]; });
                                } else if ([fields[0] isEqualToString:@"home"]) {
                                    // "home:0:0:wait" presses Android's Home key.
                                    device->press(nestvane::GuestTouch::Key::home);
                                } else if ([fields[0] isEqualToString:@"back"]) {
                                    // "back:0:0:wait" presses Android's Back key instead.
                                    device->press(nestvane::GuestTouch::Key::back);
                                } else if ([fields[0] isEqualToString:@"diagnostics"]) {
                                    // "diagnostics:0:0:wait" makes the report Export Diagnostics makes and
                                    // copies it to diagnostics.txt in Documents, without the share sheet.
                                    dispatch_async(dispatch_get_main_queue(), ^{
                                        [self collectDiagnostics:^(NSURL* report) {
                                            NSURL* copy = [documents URLByAppendingPathComponent:@"diagnostics.txt"];
                                            [NSFileManager.defaultManager removeItemAtURL:copy error:nil];
                                            [NSFileManager.defaultManager copyItemAtURL:report toURL:copy error:nil];
                                        }];
                                    });
                                } else if ([fields[0] isEqualToString:@"backup"]) {
                                    // "backup:0:0:wait" backs up as Back Up Android does, once: Android
                                    // stops and starts again. With NESTVANE_TEST_BACKUP_SAVE=1 the file
                                    // is backup-test.aar in Documents.
                                    static std::atomic_bool backedUp{false};
                                    if (!backedUp.exchange(true))
                                        dispatch_async(dispatch_get_main_queue(), ^{ [self backUp]; });
                                } else if ([fields[0] isEqualToString:@"recents"]) {
                                    // "recents:0:0:wait" presses Android's Recent Apps key.
                                    device->press(nestvane::GuestTouch::Key::recents);
                                } else if ([fields[0] isEqualToString:@"up"]) {
                                    // "up:0:0:wait" scrolls: a slow drag up the middle of the screen.
                                    for (int step = 0; step <= 20; ++step) {
                                        device->submit(axis / 2, axis * (140 - step * 4) / 200, true);
                                        usleep(30000);
                                    }
                                    usleep(200000);
                                    device->submit(axis / 2, axis * 60 / 200, false);
                                } else if ([fields[0] isEqualToString:@"drag"]) {
                                    // "drag:0:0:ms" keeps a finger moving up and down for that long, a move
                                    // every 4 ms: the list has something new to draw at every frame.
                                    const int moves = fields[3].intValue / 4;
                                    for (int move = 0; move < moves; ++move) {
                                        const int phase = move % 500, at = phase < 250 ? 140 - phase / 4 : 78 + (phase - 250) / 4;
                                        device->submit(axis / 2, axis * at / 200, true);
                                        usleep(4000);
                                    }
                                    device->submit(axis / 2, axis / 2, false);
                                    continue;
                                } else if ([fields[0] hasPrefix:@"fling"]) {
                                    // "fling:0:0:ms" and "flingdown:0:0:ms" throw the list: a fast drag up or
                                    // down, then that many milliseconds for it to coast. No frame is saved.
                                    const bool down = [fields[0] isEqualToString:@"flingdown"];
                                    for (int step = 0; step <= 8; ++step) {
                                        const int at = down ? 60 + step * 10 : 140 - step * 10;
                                        device->submit(axis / 2, axis * at / 200, true);
                                        usleep(8000);
                                    }
                                    device->submit(axis / 2, axis * (down ? 140 : 60) / 200, false);
                                    usleep(useconds_t(fields[3].intValue) * 1000);
                                    continue;
                                } else {
                                    device->submit(x, y, true);
                                    usleep(useconds_t(fields[2].intValue) * 1000);
                                    device->submit(x, y, false);
                                }
                                sleep(unsigned(fields[3].intValue));
                                save([NSString stringWithFormat:@"touch-tap-%d.png", number++]);
                            }
                        });
                    }
                }
            };
            NSURL* watchdogURL = [documents URLByAppendingPathComponent:@"guest-watchdog.txt"];
            const auto exportWatchdog = [&](std::string_view threads) {
                exportReport(watchdogURL, std::string(threads));
            };
            NSURL* liveURL = [documents URLByAppendingPathComponent:@"guest-live.txt"];
            const BOOL captureLiveFrame = [NSProcessInfo.processInfo.environment[@"NESTVANE_CAPTURE_LIVE_FRAME"]
                isEqualToString:@"1"];
            NSString* liveFrameName = [NSString stringWithFormat:@"display-live-%d.png",
                NSProcessInfo.processInfo.processIdentifier];
            NSURL* liveFrameURL = [documents URLByAppendingPathComponent:liveFrameName];
            const auto exportLiveSnapshot = [&](std::string_view threads) {
                std::string report(threads);
                AppendMemoryUsage(report);
                exportReport(liveURL, report);
                if (captureLiveFrame) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (UIImage* frame = self.display.image)
                            WriteGuestDisplayPNG(UIImagePNGRepresentation(frame), liveFrameURL);
                    });
                }
            };
            const auto runAndroid = [&]() -> nestvane::DynamicReport {
                if (!ready) {
                    NSLog(@"Unable to create guest data directory: %@", directoryError);
                    return {false, "Cannot create guest data directory\n"};
                }

                NSString* rendererPath = [NSBundle.mainBundle.privateFrameworksPath
                    stringByAppendingPathComponent:@"NestvaneGfxstream.framework/NestvaneGfxstream"];
                if (![manager fileExistsAtPath:rendererPath]) {
                    return nestvane::probeInit(initRoot.fileSystemRepresentation, database.fileSystemRepresentation,
                                               progress, {}, touch, {}, apkInbox, stop, exportWatchdog, exportLiveSnapshot);
                }

                // The code pages the last start used, read in on other cores while this one begins
                // (ios/CodePrefetch.hpp). NESTVANE_NO_PREFETCH=1 leaves it out, for comparing.
                NSString* prefetchList = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject
                    stringByAppendingPathComponent:@"boot-code-pages"];
                if (!getenv("NESTVANE_NO_PREFETCH")) {
                    const std::string imageRoot = NSBundle.mainBundle.resourcePath.fileSystemRepresentation;
                    const size_t pages = nestvane::prefetch::start(prefetchList.fileSystemRepresentation);
                    const size_t filePages = nestvane::prefetch::startFiles(
                        [prefetchList stringByAppendingString:@"-files"].fileSystemRepresentation, imageRoot);
                    NSLog(@"reading in %zu code pages and %zu pages of the system image", pages, filePages);
                }
                progress("Initializing local Android renderer");
                nestvane::GfxstreamSession renderer(rendererPath.fileSystemRepresentation, displaySize.width, displaySize.height);
                if (!renderer) return {false, renderer.error() + "\n"};
                progress("Local Android renderer initialized");
                NSURL* displayProgress = [documents URLByAppendingPathComponent:@"display-first-frame.txt"];
                const BOOL captureFirstFrame = [NSProcessInfo.processInfo.environment[@"NESTVANE_CAPTURE_FIRST_FRAME"]
                    isEqualToString:@"1"];
                auto displayReader = StartDisplayReader(renderer, displaySize, captureFirstFrame, launcherDisplayed,
                                                        ^(id frame, CGSize frameSize, bool content, bool launcher) {
                    const BOOL firstFrame = self.display.hidden;
                    [self.display show:frame size:frameSize];
                    if (const auto inbox = self->_apkInbox) [self.display setRotation:inbox->rotation.load()];
                    const auto writePresentationTime = [&](NSURL* url) {
                        const auto milliseconds = (NSProcessInfo.processInfo.systemUptime - validationStarted) * 1000;
                        NSString* message = [NSString stringWithFormat:
                            @"process %d | %.0f ms | presented %.0f × %.0f Android pixels\n",
                            NSProcessInfo.processInfo.processIdentifier, milliseconds,
                            frameSize.width, frameSize.height];
                        NSError* error = nil;
                        if (![message writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error])
                            NSLog(@"Unable to export display progress: %@", error);
                    };
                    if (firstFrame) {
                        self.startup.hidden = YES;
                        self.emblem.active = NO;
                        self.display.hidden = NO;
                        [self.view setNeedsLayout];  // viewDidLayoutSubviews says which way up Android is to be
                        self.back.hidden = self.home.hidden = self.recents.hidden = NO;
                        self.back.enabled = self.home.enabled = self.recents.enabled = YES;
                        writePresentationTime(displayProgress);
                    }
                    NSString* launcherPrefix = launcher
                        ? [NSString stringWithFormat:@"display-after-launcher-%d",
                           NSProcessInfo.processInfo.processIdentifier]
                        : nil;
                    if (launcherPrefix) {
                        writePresentationTime([documents URLByAppendingPathComponent:
                            [launcherPrefix stringByAppendingPathExtension:@"txt"]]);
                    }
                    if (captureFirstFrame && (firstFrame || content || launcher)) {
                        NSData* png = UIImagePNGRepresentation(self.display.image);
                        const auto save = [&](NSString* filename) {
                            NSURL* capture = [documents URLByAppendingPathComponent:filename];
                            WriteGuestDisplayPNG(png, capture);
                        };
                        if (firstFrame) save(@"display-first-frame.png");
                        if (launcherPrefix) save([launcherPrefix stringByAppendingPathExtension:@"png"]);
                        if (content) {
                            NSString* name = [NSString stringWithFormat:@"display-first-content-%d",
                                NSProcessInfo.processInfo.processIdentifier];
                            writePresentationTime([documents URLByAppendingPathComponent:
                                [name stringByAppendingPathExtension:@"txt"]]);
                            save([name stringByAppendingPathExtension:@"png"]);
                        }
                    }
                });
                auto boot = nestvane::probeInit(initRoot.fileSystemRepresentation, database.fileSystemRepresentation,
                                                progress, renderer.pipeFactory(), touch, nestvane::makeIOSCamera(),
                                                apkInbox, stop, exportWatchdog, exportLiveSnapshot, renderer.colorDma());
                boot.report.insert(0, "Local Android renderer initialized\n");
                return boot;
            };
            auto probe = runAndroid();
            failedBeforeHome = !probe.passed && !stop.stop_requested() && !launcherDisplayed.load();
            result.report += "\n" + probe.report;
            AppendMemoryUsage(result.report);
            if (!probe.passed) status = @"Android 17 startup probe stopped";
            else if (probe.report.find("linkerconfig generation verified: yes") != std::string::npos)
                status = @"Android 17 linker configuration generated";
            else if (probe.report.find("Runtime APEX activation verified: yes") != std::string::npos)
                status = @"Android 17 Runtime APEX activated";
            else status = @"Android 17 init and properties verified";
        }
        if (apkInbox) apkInbox->stop();
        if (artStatus) status = artStatus;
        if (stop.stop_requested()) status = @"Android stopped";
        NSString* report = exportReport(outputURL, result.report);

        dispatch_async(dispatch_get_main_queue(), ^{
            self.status.text = status;
            [self.display setTouchDevice:std::shared_ptr<nestvane::GuestTouch>{}];
            self.display.hidden = YES;
            [self.display show:nil size:CGSizeZero];
            self.startup.hidden = NO;
            self.emblem.active = YES;
    self.emblem.active = YES;
            self.details.text = apkInbox ? nil : report;
            self.details.hidden = bool(apkInbox);
            self.back.enabled = self.home.enabled = self.recents.enabled = NO;
            self->_androidStop = std::stop_source{std::nostopstate};
            self.importApk.enabled = NO;
            [self setRunSymbol:@"play.fill" label:apkInbox ? @"Start Android" : @"Run validation"];
            self.run.enabled = YES;
            UIApplication.sharedApplication.idleTimerDisabled = NO;
            // Android did not start from a restored backup: what it had before is put back.
            if (failedBeforeHome && nestvane::backup::restorePending()) {
                if (NSDictionary* settings = nestvane::backup::rollBack()) {
                    ApplyAndroidSettings(settings);
                    self.status.text = @"Android did not start from the backup. What it had before is back.";
                }
            }
            if (void (^after)(void) = std::exchange(self->_afterStop, nil)) after();
        });
    }];
    guestThread.qualityOfService = NSQualityOfServiceUserInitiated;
    guestThread.stackSize = 8 << 20;
    guestThread.name = @"Android guest";
    [guestThread start];
}
@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [ValidationViewController new];
    [self.window makeKeyAndVisible];
    [self scene:scene openURLContexts:options.URLContexts];
    if ([options.shortcutItem.type isEqualToString:InstallApkShortcut])
        [(ValidationViewController*)self.window.rootViewController chooseApkWhenReady];
}

// An .apk opened with this app from Files, AirDrop or another app's share sheet.
- (void)scene:(UIScene*)scene openURLContexts:(NSSet<UIOpenURLContext*>*)contexts {
    for (UIOpenURLContext* context in contexts) {
        NSLog(@"APK: asked to open %@", context.URL.lastPathComponent);
        if (!context.URL.isFileURL) continue;
        [(ValidationViewController*)self.window.rootViewController installApkAtURL:context.URL];
        break;
    }
}

// "Install APK" in the menu of the app's icon on the home screen.
- (void)windowScene:(UIWindowScene*)scene performActionForShortcutItem:(UIApplicationShortcutItem*)item
  completionHandler:(void (^)(BOOL))completion {
    const BOOL known = [item.type isEqualToString:InstallApkShortcut];
    if (known) [(ValidationViewController*)self.window.rootViewController chooseApkWhenReady];
    completion(known);
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
@end

// Android's storage is the device's: the size of the volume the app is on and the space iOS has for
// important use on it, the numbers the device's Settings shows (statfs leaves out purgeable space).
static nestvane::ProbeLinux::HostVolume deviceVolume() {
    @autoreleasepool {
        // A new URL each time: resource values are cached on the URL.
        NSURL* home = [NSURL fileURLWithPath:NSHomeDirectory() isDirectory:YES];
        NSDictionary* values = [home resourceValuesForKeys:@[NSURLVolumeTotalCapacityKey,
                                                             NSURLVolumeAvailableCapacityForImportantUsageKey]
                                                     error:nil];
        NSNumber* total = values[NSURLVolumeTotalCapacityKey];
        NSNumber* available = values[NSURLVolumeAvailableCapacityForImportantUsageKey];
        if (!total || !available || available.longLongValue < 0) return {};
        return {total.unsignedLongLongValue, available.unsignedLongLongValue};
    }
}

int main(int argc, char* argv[]) {
    // NESTVANE_ADDRESS_MAP=1: before Android starts, the limits iOS gave this process (memory, address
    // range) and how much address space can still be reserved, in 64 MiB pieces, given back at once.
    if (const char* map = getenv("NESTVANE_ADDRESS_MAP"); map && std::string_view(map) == "1") {
        std::string report;
        AppendMemoryUsage(report);
        std::vector<vm_address_t> pieces;
        for (;;) {
            vm_address_t piece = 0;
            if (vm_allocate(mach_task_self(), &piece, 64ull << 20, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) break;
            pieces.push_back(piece);
            if (pieces.size() >= 16384) break;
        }
        report += "  reservable before Android: " + std::to_string(pieces.size() * (64ull << 20)) + " bytes\n";
        for (const auto piece : pieces) vm_deallocate(mach_task_self(), piece, 64ull << 20);
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        [[NSData dataWithBytes:report.data() length:report.size()]
            writeToURL:[documents URLByAppendingPathComponent:@"memory-limits.txt"] atomically:YES];
    }
    nestvane::ProbeLinux::setHostVolumeSource(deviceVolume);
    // Android's codec service decodes H.264 and HEVC with the phone's decoders (NESTVANE_NO_HARDWARE_VIDEO
    // leaves them out: the service then finds no media device and stops, and Android decodes in software).
    if (!getenv("NESTVANE_NO_HARDWARE_VIDEO")) nestvane::ProbeLinux::setVideoDecoders(nestvane::makeIOSVideoDecoders());
    // NESTVANE_TEST_ROOT=allow or =deny turns root on and answers for every app that has no answer
    // yet, instead of asking, for the app's tests.
    nestvane::ProbeLinux::setRootPolicy({
        [] { return [NSUserDefaults.standardUserDefaults boolForKey:RootEnabledKey] || getenv("NESTVANE_TEST_ROOT"); },
        [](uint32_t uid, const std::string& package) -> int {
            @autoreleasepool {
                NSString* name = package.empty() ? [NSString stringWithFormat:@"uid %u", uid] : @(package.c_str());
                NSNumber* answer = [NSUserDefaults.standardUserDefaults objectForKey:[RootGrantPrefix stringByAppendingString:name]];
                if (answer) return answer.boolValue ? 1 : 0;
                if (const char* test = getenv("NESTVANE_TEST_ROOT")) return std::string_view(test) == "allow" ? 1 : 0;
                AskForRoot(name);
                return -1;
            }
        },
        // Every answer, and a new one from the KernelSU Manager: the same ones Settings lists.
        []() {
            __block std::vector<std::pair<std::string, bool>> answers;
            @autoreleasepool {
                [NSUserDefaults.standardUserDefaults.dictionaryRepresentation enumerateKeysAndObjectsUsingBlock:^(NSString* key, id value, BOOL*) {
                    if ([key hasPrefix:RootGrantPrefix] && [value isKindOfClass:NSNumber.class])
                        answers.emplace_back([key substringFromIndex:RootGrantPrefix.length].UTF8String, [value boolValue]);
                }];
            }
            return answers;
        },
        [](const std::string& package, bool allowed) {
            @autoreleasepool {
                [NSUserDefaults.standardUserDefaults setBool:allowed forKey:[RootGrantPrefix stringByAppendingString:@(package.c_str())]];
            }
        },
        // The KernelSU Manager's whole profile for an app, kept so that it reads back what it set.
        [](const std::string& package) {
            @autoreleasepool {
                NSData* data = [NSUserDefaults.standardUserDefaults dataForKey:[RootProfilePrefix stringByAppendingString:@(package.c_str())]];
                const auto* bytes = static_cast<const uint8_t*>(data.bytes);
                return data ? std::vector<uint8_t>(bytes, bytes + data.length) : std::vector<uint8_t>{};
            }
        },
        [](const std::string& package, std::span<const uint8_t> profile) {
            @autoreleasepool {
                [NSUserDefaults.standardUserDefaults setObject:[NSData dataWithBytes:profile.data() length:profile.size()]
                                                        forKey:[RootProfilePrefix stringByAppendingString:@(package.c_str())]];
            }
        }});
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
