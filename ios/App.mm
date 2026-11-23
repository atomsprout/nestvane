#import <CoreGraphics/CoreGraphics.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Network/Network.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#include "GuestAudio.hpp"
#include "GuestBattery.hpp"
#include "DynamicProbe.hpp"
#include "GfxstreamPipe.hpp"
#include "GfxstreamSession.hpp"
#include "HostSocket.hpp"
#include "IOSCamera.hpp"
#include "Validation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <mach/mach.h>
#include <memory>
#include <new>
#include <thread>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <vector>

// Where the output of Android's sound stands, in words, for the rate report (NESTVANE_DISPLAY_RATE).
static NSString* GuestAudioOutputState = @"not started";

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

DisplaySize GuestDisplaySize(CGSize viewport) {
    if (viewport.width <= 0 || viewport.height <= 0) return {defaultGuestWidth, defaultGuestHeight};
    const double aspect = std::clamp(double(viewport.width / viewport.height), 0.25, 4.0);
    const auto evenPixels = [](double length) { return uint32_t(2 * std::lround(length / 2)); };
    return {evenPixels(std::sqrt(displayPixelBudget * aspect)),
            evenPixels(std::sqrt(displayPixelBudget / aspect))};
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
                [rates appendFormat:@"%@%dx%d at %d Hz: composed %.0f, shown %.0f frames per second; sound %.0f bytes a second taken, %.0f played by Android, peak %u (%@, %zu bytes waiting); most in a tenth %llu\n",
                    refresh ? [NSString stringWithFormat:@"surfaces, screen refreshed %.0f times a second; ", [refresh takeTicks] / seconds] : @"read back; ",
                    size.width, size.height, hertz,
                    rateFirstFrame ? double(lastFrame - rateFirstFrame + 1) / seconds : 0.0, shown / seconds, double(soundNow - soundBefore) / seconds, double(playedNow - playedBefore) / seconds,
                    sound.peak.exchange(0), GuestAudioOutputState, sound.buffered(), std::exchange(tenthMost, 0)];
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
}
} // namespace

// The rate Android composes at and the app looks for frames at: 60 a second, or 120 on a screen that
// shows 120 when the person has turned that on in Settings. The framework draws at 120 only because
// the overlay in fixtures/overlay-framework lifts its default of 60. NESTVANE_DISPLAY_HZ, when the app
// was launched with it, overrides both; Android reads the rate once, as it starts.
static NSString* const Display120HzKey = @"Display120Hz";
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
- (void)setTouchDevice:(std::shared_ptr<nestvane::GuestTouch>)device;
- (void)pressKey:(nestvane::GuestTouch::Key)key;
@end

@implementation GuestDisplayView {
    std::shared_ptr<nestvane::GuestTouch> _touchDevice;
    UITouch* _activeTouch;
    id _frame;
    CGSize _frameSize;
}

- (void)show:(id)frame size:(CGSize)size {
    _frame = frame;
    _frameSize = frame ? size : CGSizeZero;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self.layer.contents = [frame isKindOfClass:UIImage.class] ? (__bridge id)static_cast<UIImage*>(frame).CGImage : frame;
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
    const CGSize imageSize = _frameSize;
    const CGRect bounds = self.bounds;
    if (!_touchDevice || imageSize.width <= 0 || imageSize.height <= 0 ||
        bounds.size.width <= 0 || bounds.size.height <= 0) return NO;
    const CGFloat scale = std::min(bounds.size.width / imageSize.width, bounds.size.height / imageSize.height);
    const CGSize size = CGSizeMake(imageSize.width * scale, imageSize.height * scale);
    const CGPoint location = [touch locationInView:self];
    const CGFloat x = (location.x - CGRectGetMidX(bounds) + size.width / 2) / size.width;
    const CGFloat y = (location.y - CGRectGetMidY(bounds) + size.height / 2) / size.height;
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
        if (!wanted) {
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
    if (![AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:&error] ||
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
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                           target:self action:@selector(close)];
    NSURL* storage = [[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject
        URLByAppendingPathComponent:@"Android"];
    NSString* files = [@"shareddocuments://" stringByAppendingString:
        [storage.path stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet]];
    NSString* rate = [NSString stringWithFormat:@"%s frames per second", getenv("NESTVANE_DISPLAY_HZ") ?: "60"];
    const BOOL fast = UIScreen.mainScreen.maximumFramesPerSecond >= 120;
    const auto row = [](NSString* symbol, NSString* title, NSString* detail, NSString* url) {
        return @{@"symbol": symbol, @"title": title, @"detail": detail ?: @"", @"url": url ?: @""};
    };
    _sections = @[
        @{@"title": @"Android", @"footer": fast
              ? @"120 Hz makes motion smoother and uses more battery. It takes effect the next time Android starts, as do files you put into the Android folder."
              : @"Files you put into the Android folder appear in Android the next time it starts.",
          @"rows": [@[row(@"cpu", @"Android version", @"17", nil), row(@"display", @"Display", rate, nil)]
              arrayByAddingObjectsFromArray:[(fast ? @[@{@"symbol": @"gauge.with.dots.needle.67percent", @"title": @"120 Hz",
                                                        @"detail": @"", @"url": @"", @"switch": Display120HzKey}] : @[])
              arrayByAddingObject:row(@"folder", @"Shared files", @"Open in Files", files)]]},
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
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* details;
@property(nonatomic, strong) GuestDisplayView* display;
@property(nonatomic, strong) UIButton* run;
@property(nonatomic, strong) UIButton* importApk;
@property(nonatomic, strong) UIButton* more;
@property(nonatomic, strong) UIButton* back;
@property(nonatomic, strong) UIButton* home;
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
    self.importApk = ToolbarButton(@"square.and.arrow.down", @"Install APK…", self, @selector(chooseApk));
    self.run = ToolbarButton(@"play.fill", @"Start Android", self, @selector(runOrStop));
    // Back and Home are used all the time and sit alone in the bar. What is used now and then, and
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
            completion(@[install, text, settings, bug, power]);
        }]]];
    UIStackView* controls = [[UIStackView alloc] initWithArrangedSubviews:
        @[self.back, self.home, self.more, self.run]];
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
    if (NSString* wanted = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_SCREEN"]) {
        __weak ValidationViewController* shown = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
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
    AskForUpdate(^(NSString* version) {
        [owner showApkStatus:[NSString stringWithFormat:@"Nestvane %@ is available. See Settings.", version] transient:YES];
    });
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

- (void)chooseApk {
    if (!_apkInbox || !self.importApk.enabled) return;
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[UTTypeData] asCopy:YES];  // not every provider tags .apk files with a type
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    if (NSURL* source = urls.firstObject) [self installApkAtURL:source];
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
    if ([source.pathExtension caseInsensitiveCompare:@"apk"] != NSOrderedSame) {
        [self showApkStatus:@"Choose an .apk file." transient:YES];
        return;
    }
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
            report(@"Choose an APK file up to 256 MiB.");
            return;
        }
        const auto* bytes = static_cast<const uint8_t*>(data.bytes);
        std::vector<uint8_t> file(bytes, bytes + data.length);
        if (access) [source stopAccessingSecurityScopedResource];
        // A file another app handed over was copied into this app's inbox directory for it.
        if ([source.path containsString:@"/Documents/Inbox/"]) [NSFileManager.defaultManager removeItemAtURL:source error:nil];
        if (!inbox->submit(std::move(file), [report](Inbox::Result result) {
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
    // The app declares iOS 26 as its minimum, and iOS itself refuses to install it on anything older.
    // A sideloading tool can take that declaration out; then the app says so here and starts nothing,
    // since it has never been run on an older system. NESTVANE_TEST_OLD_SYSTEM=1 shows the message.
    if (NSProcessInfo.processInfo.operatingSystemVersion.majorVersion < 26 || getenv("NESTVANE_TEST_OLD_SYSTEM")) {
        self.status.text = [NSString stringWithFormat:@"Nestvane needs iOS 26 or later.\nThis iPhone has iOS %@.",
            UIDevice.currentDevice.systemVersion];
        self.status.numberOfLines = 0;
        self.status.textAlignment = NSTextAlignmentCenter;
        return;
    }
    NSString* installer = [NSBundle.mainBundle.resourcePath
        stringByAppendingPathComponent:@"android17-init-root/system/bin/nestvane-apk-install"];
    _apkInbox = [NSFileManager.defaultManager fileExistsAtPath:installer] ?
        std::make_shared<nestvane::GuestApkInbox>() : nullptr;
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
    self.back.hidden = self.home.hidden = !apkInbox;
    self.back.enabled = self.home.enabled = NO;
    self.startup.hidden = NO;
    self.emblem.active = YES;
    self.details.hidden = YES;
    self.details.text = nil;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    self.status.text = apkInbox ? @"Starting Android…" : @"Running ARM64 ELF validation…";
    const auto displaySize = GuestDisplaySize(self.display.bounds.size);
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
            // Android's sound comes out of the phone.
            static dispatch_source_t audioWatch;
            static dispatch_once_t audioWatched;
            dispatch_once(&audioWatched, ^{
                audioWatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
                dispatch_source_set_timer(audioWatch, DISPATCH_TIME_NOW, NSEC_PER_SEC / 4, NSEC_PER_SEC / 20);
                dispatch_source_set_event_handler(audioWatch, ^{ FollowGuestAudio(); });
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
                    // NESTVANE_TEST_TEXT="<sender>|<text>" hands Android a text message, as the menu does,
                    // ten seconds after the launcher is displayed.
                    if (NSString* message = NSProcessInfo.processInfo.environment[@"NESTVANE_TEST_TEXT"]; message && apkInbox) {
                        NSArray<NSString*>* parts = [message componentsSeparatedByString:@"|"];
                        if (parts.count == 2)
                            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                                           dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                                apkInbox->submitText(parts[0].UTF8String, parts[1].UTF8String);
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
                                for (NSString* name in [names componentsSeparatedByString:@","]) {
                                    NSData* data = [NSData dataWithContentsOfURL:[documents URLByAppendingPathComponent:name]];
                                    const auto* bytes = static_cast<const uint8_t*>(data.bytes);
                                    const auto submitted = NSProcessInfo.processInfo.systemUptime;
                                    dispatch_semaphore_t done = dispatch_semaphore_create(0);
                                    const auto outcome = std::make_shared<std::atomic_int>(-1);
                                    const bool accepted = data.length && apkInbox->submit(std::vector<uint8_t>(bytes, bytes + data.length),
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
                                if ([fields[0] isEqualToString:@"back"]) {
                                    // "back:0:0:wait" presses Android's Back key instead.
                                    device->press(nestvane::GuestTouch::Key::back);
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
                        self.back.hidden = self.home.hidden = NO;
                        self.back.enabled = self.home.enabled = YES;
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
            self.back.enabled = self.home.enabled = NO;
            self->_androidStop = std::stop_source{std::nostopstate};
            self.importApk.enabled = NO;
            [self setRunSymbol:@"play.fill" label:apkInbox ? @"Start Android" : @"Run validation"];
            self.run.enabled = YES;
            UIApplication.sharedApplication.idleTimerDisabled = NO;
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

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
