#import <CoreGraphics/CoreGraphics.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "DynamicProbe.hpp"
#include "GfxstreamPipe.hpp"
#include "GfxstreamSession.hpp"
#include "IOSCamera.hpp"
#include "Validation.hpp"

#include <atomic>
#include <chrono>
#include <mach/mach.h>
#include <thread>
#include <sys/resource.h>
#include <vector>

namespace {
constexpr uint32_t guestWidth = 640;
constexpr uint32_t guestHeight = 960;

UIImage* GuestDisplayImage(std::span<const uint8_t> pixels, uint32_t width, uint32_t height) {
    const size_t stride = size_t(width) * 4;
    const size_t bytes = stride * height;
    if (!width || !height || width > 4096 || height > 4096 || bytes > pixels.size()) return nil;

    // The guest compositor has already oriented its output for the display.
    NSData* frameBytes = [NSData dataWithBytes:pixels.data() length:bytes];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)frameBytes);
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGBitmapInfo bitmap = kCGBitmapByteOrder32Big | CGBitmapInfo(kCGImageAlphaNoneSkipLast);
    CGImageRef frame = provider && color
        ? CGImageCreate(width, height, 8, 32, stride, color, bitmap, provider, nullptr, false,
                        kCGRenderingIntentDefault)
        : nullptr;
    UIImage* image = frame ? [UIImage imageWithCGImage:frame] : nil;
    if (frame) CGImageRelease(frame);
    if (color) CGColorSpaceRelease(color);
    if (provider) CGDataProviderRelease(provider);
    return image;
}

std::jthread StartDisplayReader(const nestvane::GfxstreamSession& renderer, bool captureContent,
                                const std::atomic_bool& launcherDrawn,
                                void (^present)(UIImage*, bool, bool)) {
    if (!renderer.hasDisplayReadback()) return {};
    return std::jthread([&renderer, &launcherDrawn, captureContent, present](std::stop_token stop) mutable {
        bool captureLauncher = captureContent;
        std::vector<uint8_t> pixels(size_t(guestWidth) * guestHeight * 4);
        const auto slot = dispatch_semaphore_create(1);
        uint64_t lastFrame = 0;
        while (!stop.stop_requested()) {
            @autoreleasepool {
                if (dispatch_semaphore_wait(slot, DISPATCH_TIME_NOW) == 0) {
                    const bool wantLauncher = captureLauncher && launcherDrawn.load(std::memory_order_relaxed);
                    uint32_t dimensions[2]{};
                    const auto frame = renderer.readDisplay(lastFrame, pixels, dimensions);
                    UIImage* image = frame ? GuestDisplayImage(pixels, dimensions[0], dimensions[1]) : nil;
                    if (image) {
                        lastFrame = frame;
                        const auto bytes = size_t(dimensions[0]) * dimensions[1] * 4;
                        const bool nonuniform = (captureContent || wantLauncher) &&
                            nestvane::guestDisplayHasColorVariation(std::span(pixels).first(bytes));
                        const bool content = captureContent && nonuniform;
                        const bool launcher = wantLauncher && nonuniform;
                        if (content) captureContent = false;
                        if (launcher) captureLauncher = false;
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
@property(nonatomic, strong) UILabel* apkStatus;
@end

@implementation ValidationViewController {
    std::shared_ptr<nestvane::GuestApkInbox> _apkInbox;
    std::stop_source _androidStop;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;

    UILabel* title = [UILabel new];
    title.text = @"nestvane";
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleLargeTitle];
    title.adjustsFontForContentSizeCategory = YES;

    self.status = [UILabel new];
    self.status.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    self.status.numberOfLines = 0;

    self.details = [UITextView new];
    self.details.editable = NO;
    self.details.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    self.details.backgroundColor = UIColor.secondarySystemBackgroundColor;
    self.details.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);

    self.display = [GuestDisplayView new];
    self.display.userInteractionEnabled = YES;
    self.display.contentMode = UIViewContentModeScaleAspectFit;
    self.display.backgroundColor = UIColor.blackColor;
    self.display.hidden = YES;
    self.display.accessibilityLabel = @"Android display";
    self.display.isAccessibilityElement = YES;
    [self.display setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                                  forAxis:UILayoutConstraintAxisVertical];
    [self.display setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                                  forAxis:UILayoutConstraintAxisHorizontal];

    self.run = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.run setTitle:@"Run validation" forState:UIControlStateNormal];
    [self.run addTarget:self action:@selector(runOrStop) forControlEvents:UIControlEventTouchUpInside];

    self.importApk = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.importApk setTitle:@"Install APK…" forState:UIControlStateNormal];
    [self.importApk addTarget:self action:@selector(chooseApk) forControlEvents:UIControlEventTouchUpInside];
    self.importApk.hidden = YES;

    self.apkStatus = [UILabel new];
    self.apkStatus.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    self.apkStatus.adjustsFontForContentSizeCategory = YES;
    self.apkStatus.numberOfLines = 0;
    self.apkStatus.hidden = YES;

    UIStackView* layout = [[UIStackView alloc] initWithArrangedSubviews:
        @[title, self.status, self.details, self.display, self.apkStatus, self.importApk, self.run]];
    layout.axis = UILayoutConstraintAxisVertical;
    layout.spacing = 12;
    layout.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:layout];

    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [layout.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20],
        [layout.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20],
        [layout.topAnchor constraintEqualToAnchor:safe.topAnchor constant:16],
        [layout.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
    ]];
    [self runValidation];
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
    self.apkStatus.hidden = NO;
    if ([source.pathExtension caseInsensitiveCompare:@"apk"] != NSOrderedSame) {
        self.apkStatus.text = @"Choose an .apk file.";
        return;
    }
    self.importApk.enabled = NO;
    self.apkStatus.text = @"Waiting for Android to install the APK…";
    __weak ValidationViewController* weakSelf = self;
    const std::weak_ptr<nestvane::GuestApkInbox> active = inbox;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        using Inbox = nestvane::GuestApkInbox;
        const auto report = [weakSelf, active](NSString* message) {
            dispatch_async(dispatch_get_main_queue(), ^{
                ValidationViewController* owner = weakSelf;
                const auto current = active.lock();
                if (!owner || !current || owner->_apkInbox != current) return;
                owner.apkStatus.text = message;
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
        [self.run setTitle:@"Stop Android" forState:UIControlStateNormal];
        self.run.enabled = YES;
    }
    self.importApk.hidden = !apkInbox;
    self.importApk.enabled = bool(apkInbox);
    self.apkStatus.hidden = YES;
    auto touch = std::make_shared<nestvane::GuestTouch>();
    [self.display setTouchDevice:touch];
    self.display.image = nil;
    self.display.hidden = YES;
    self.details.hidden = NO;
    self.details.text = nil;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    self.status.text = @"Running ARM64 ELF validation…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* before = ThermalState();
        NSString* resources = NSBundle.mainBundle.resourcePath;
        auto result = nestvane::validate(resources.fileSystemRepresentation);
        NSString* status = result.passed ? @"Execution paths agree" : @"Validation failed";
        bool previousChecksPassed = result.passed;
        if (previousChecksPassed && [NSFileManager.defaultManager fileExistsAtPath:
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
            std::string milestones;
            std::atomic_bool launcherDisplayed{false};
            NSURL* launcherURL = [documents URLByAppendingPathComponent:@"launcher-displayed.txt"];
            const auto progress = [&](std::string_view event) {
                const auto elapsedSeconds = NSProcessInfo.processInfo.systemUptime - bootStarted;
                milestones += "[" + std::to_string(static_cast<long long>(elapsedSeconds * 1000)) + " ms] ";
                milestones.append(event);
                milestones += '\n';
                constexpr size_t maxMilestoneBytes = 16 * 1024;
                if (milestones.size() > maxMilestoneBytes)
                    milestones.erase(0, milestones.find('\n', milestones.size() - maxMilestoneBytes) + 1);
                exportReport(progressURL, milestones);
                if (event.starts_with("ActivityTaskManager: Displayed com.android.launcher3/") &&
                    !launcherDisplayed.exchange(true, std::memory_order_relaxed)) {
                    const auto elapsed = (NSProcessInfo.processInfo.systemUptime - validationStarted) * 1000;
                    std::string report = "Android Launcher displayed after " +
                        std::to_string(static_cast<long long>(elapsed)) + " ms\n";
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
            const auto exportLiveSnapshot = [&](std::string_view threads) {
                std::string report(threads);
                AppendMemoryUsage(report);
                exportReport(liveURL, report);
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
                nestvane::GfxstreamSession renderer(rendererPath.fileSystemRepresentation, guestWidth, guestHeight);
                if (!renderer) return {false, renderer.error() + "\n"};
                progress("Local Android renderer initialized");
                NSURL* displayProgress = [documents URLByAppendingPathComponent:@"display-first-frame.txt"];
                const BOOL captureFirstFrame = [NSProcessInfo.processInfo.environment[@"NESTVANE_CAPTURE_FIRST_FRAME"]
                    isEqualToString:@"1"];
                auto displayReader = StartDisplayReader(renderer, captureFirstFrame, launcherDisplayed,
                                                        ^(UIImage* frame, bool content, bool launcher) {
                    const BOOL firstFrame = self.display.hidden;
                    self.display.image = frame;
                    const auto writePresentationTime = [&](NSURL* url) {
                        const auto milliseconds = (NSProcessInfo.processInfo.systemUptime - bootStarted) * 1000;
                        NSString* message = [NSString stringWithFormat:
                            @"process %d | %.0f ms | presented %.0f × %.0f Android pixels\n",
                            NSProcessInfo.processInfo.processIdentifier, milliseconds,
                            frame.size.width, frame.size.height];
                        NSError* error = nil;
                        if (![message writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error])
                            NSLog(@"Unable to export display progress: %@", error);
                    };
                    if (firstFrame) {
                        self.status.text = @"Android 17 display";
                        self.details.hidden = YES;
                        self.display.hidden = NO;
                        writePresentationTime(displayProgress);
                    }
                    if (captureFirstFrame && (firstFrame || content || launcher)) {
                        NSData* png = UIImagePNGRepresentation(frame);
                        const auto save = [&](NSString* filename) {
                            NSURL* capture = [documents URLByAppendingPathComponent:filename];
                            NSError* error = nil;
                            if (!png || ![png writeToURL:capture options:NSDataWritingAtomic error:&error])
                                NSLog(@"Unable to export %@: %@", filename, error);
                        };
                        if (firstFrame) save(@"display-first-frame.png");
                        if (launcher) {
                            NSString* name = [NSString stringWithFormat:@"display-after-launcher-%d",
                                NSProcessInfo.processInfo.processIdentifier];
                            writePresentationTime([documents URLByAppendingPathComponent:
                                [name stringByAppendingPathExtension:@"txt"]]);
                            save([name stringByAppendingPathExtension:@"png"]);
                        }
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
                                                apkInbox, stop, exportWatchdog, exportLiveSnapshot);
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
            self.details.text = report;
            self->_androidStop = std::stop_source{std::nostopstate};
            self.importApk.enabled = NO;
            [self.run setTitle:apkInbox ? @"Start Android" : @"Run validation" forState:UIControlStateNormal];
            self.run.enabled = YES;
            UIApplication.sharedApplication.idleTimerDisabled = NO;
        });
    });
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
