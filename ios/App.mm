#import <CoreGraphics/CoreGraphics.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "DynamicProbe.hpp"
#include "GfxstreamPipe.hpp"
#include "GfxstreamSession.hpp"
#include "IOSCamera.hpp"
#include "Validation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <mach/mach.h>
#include <memory>
#include <new>
#include <thread>
#include <sys/resource.h>
#include <vector>

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
                                void (^present)(UIImage*, bool, bool)) {
    if (!renderer.hasDisplayReadback()) return {};
    return std::jthread([&renderer, &launcherDrawn, size, captureContent, present](std::stop_token stop) mutable {
        bool reportLauncher = true;
        const auto capacity = std::max(displayPixelBudget, size_t(size.width) * size.height) * 4;
        std::unique_ptr<uint8_t[]> pixels;
        const auto slot = dispatch_semaphore_create(1);
        uint64_t lastFrame = 0;
        while (!stop.stop_requested()) {
            @autoreleasepool {
                if (dispatch_semaphore_wait(slot, DISPATCH_TIME_NOW) == 0) {
                    const bool wantLauncher = reportLauncher && launcherDrawn.load(std::memory_order_relaxed);
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
                        lastFrame = frame;
                        const auto bytes = size_t(dimensions[0]) * dimensions[1] * 4;
                        const bool nonuniform = (captureContent || wantLauncher) &&
                            nestvane::guestDisplayHasColorVariation({readback, bytes});
                        const bool content = captureContent && nonuniform;
                        const bool launcher = wantLauncher && nonuniform;
                        if (content) captureContent = false;
                        if (launcher) reportLauncher = false;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            present(image, content, launcher);
                            dispatch_semaphore_signal(slot);
                        });
                    } else {
                        dispatch_semaphore_signal(slot);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
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

static NSString* ThermalState() {
    switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateNominal: return @"nominal";
        case NSProcessInfoThermalStateFair: return @"fair";
        case NSProcessInfoThermalStateSerious: return @"serious";
        case NSProcessInfoThermalStateCritical: return @"critical";
    }
    return @"unknown";
}

@interface GuestDisplayView : UIImageView
- (void)setTouchDevice:(std::shared_ptr<nestvane::GuestTouch>)device;
- (void)pressKey:(nestvane::GuestTouch::Key)key;
@end

@implementation GuestDisplayView {
    std::shared_ptr<nestvane::GuestTouch> _touchDevice;
    UITouch* _activeTouch;
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
    const CGSize imageSize = self.image.size;
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

@interface ValidationViewController : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* details;
@property(nonatomic, strong) GuestDisplayView* display;
@property(nonatomic, strong) UIButton* run;
@property(nonatomic, strong) UIButton* importApk;
@property(nonatomic, strong) UIButton* back;
@property(nonatomic, strong) UIButton* home;
@property(nonatomic, strong) UIStackView* startup;
@property(nonatomic, strong) UILabel* apkStatus;
@end

@implementation ValidationViewController {
    std::shared_ptr<nestvane::GuestApkInbox> _apkInbox;
    std::stop_source _androidStop;
    BOOL _started;
    NSUInteger _apkStatusGeneration;
}

- (BOOL)prefersStatusBarHidden {
    return YES;
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
    UIStackView* controls = [[UIStackView alloc] initWithArrangedSubviews:
        @[self.back, self.home, self.importApk, self.run]];
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

    self.startup = [[UIStackView alloc] initWithArrangedSubviews:@[self.status, self.details]];
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
    [self runValidation];
}

- (void)setRunSymbol:(NSString*)symbol label:(NSString*)label {
    UIButtonConfiguration* configuration = self.run.configuration;
    configuration.image = [UIImage systemImageNamed:symbol];
    self.run.configuration = configuration;
    self.run.accessibilityLabel = label;
}

- (void)showApkStatus:(NSString*)message transient:(BOOL)transient {
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
        initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    const auto inbox = _apkInbox;
    NSURL* source = urls.firstObject;
    if (!inbox || !source) return;
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
        NSNumber* size = nil;
        NSNumber* regular = nil;
        const BOOL readable = [source getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil] &&
            [source getResourceValue:&size forKey:NSURLFileSizeKey error:nil] && regular.boolValue;
        if (!readable || !size.unsignedLongLongValue || size.unsignedLongLongValue > Inbox::maxBytes) {
            if (access) [source stopAccessingSecurityScopedResource];
            report(@"Choose an APK file up to 64 MiB.");
            return;
        }
        NSData* data = [NSData dataWithContentsOfURL:source options:NSDataReadingMappedIfSafe error:nil];
        if (!data || data.length != size.unsignedLongLongValue) {
            if (access) [source stopAccessingSecurityScopedResource];
            report(@"Couldn’t read the selected APK.");
            return;
        }
        const auto* bytes = static_cast<const uint8_t*>(data.bytes);
        std::vector<uint8_t> file(bytes, bytes + data.length);
        if (access) [source stopAccessingSecurityScopedResource];
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
    NSString* installer = [NSBundle.mainBundle.resourcePath
        stringByAppendingPathComponent:@"android17-init-root/system/bin/nestvane-apk-install"];
    _apkInbox = [NSFileManager.defaultManager fileExistsAtPath:installer] ?
        std::make_shared<nestvane::GuestApkInbox>() : nullptr;
    const auto apkInbox = _apkInbox;
    _androidStop = apkInbox ? std::stop_source{} : std::stop_source{std::nostopstate};
    const auto stop = _androidStop.get_token();
    if (stop.stop_possible()) {
        [self setRunSymbol:@"stop.fill" label:@"Stop Android"];
        self.run.enabled = YES;
    }
    self.importApk.hidden = !apkInbox;
    self.importApk.enabled = bool(apkInbox);
    self.apkStatus.hidden = YES;
    auto touch = std::make_shared<nestvane::GuestTouch>();
    [self.display setTouchDevice:touch];
    self.display.image = nil;
    self.display.hidden = YES;
    self.back.hidden = self.home.hidden = !apkInbox;
    self.back.enabled = self.home.enabled = NO;
    self.startup.hidden = NO;
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
            std::atomic_bool launcherDisplayed{false};
            NSURL* launcherURL = [documents URLByAppendingPathComponent:@"launcher-displayed.txt"];
            const auto progress = [&](std::string_view event) {
                const auto now = NSProcessInfo.processInfo.systemUptime;
                const auto elapsed = static_cast<long long>((now - validationStarted) * 1000);
                milestones += "[" + std::to_string(static_cast<long long>((now - bootStarted) * 1000)) + " ms] ";
                milestones.append(event);
                milestones += '\n';
                constexpr size_t maxMilestoneBytes = 16 * 1024;
                if (milestones.size() > maxMilestoneBytes)
                    milestones.erase(0, milestones.find('\n', milestones.size() - maxMilestoneBytes) + 1);
                exportReport(progressURL, milestones);
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
                                                        ^(UIImage* frame, bool content, bool launcher) {
                    const BOOL firstFrame = self.display.hidden;
                    self.display.image = frame;
                    const auto writePresentationTime = [&](NSURL* url) {
                        const auto milliseconds = (NSProcessInfo.processInfo.systemUptime - validationStarted) * 1000;
                        NSString* message = [NSString stringWithFormat:
                            @"process %d | %.0f ms | presented %.0f × %.0f Android pixels\n",
                            NSProcessInfo.processInfo.processIdentifier, milliseconds,
                            frame.size.width, frame.size.height];
                        NSError* error = nil;
                        if (![message writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error])
                            NSLog(@"Unable to export display progress: %@", error);
                    };
                    if (firstFrame) {
                        self.startup.hidden = YES;
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
                        NSData* png = UIImagePNGRepresentation(frame);
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
            self.display.image = nil;
            self.startup.hidden = NO;
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
