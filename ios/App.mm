#import <CoreGraphics/CoreGraphics.h>
#import <UIKit/UIKit.h>

#include "DynamicProbe.hpp"
#include "GfxstreamPipe.hpp"
#include "GfxstreamSession.hpp"
#include "Validation.hpp"

#include <chrono>
#include <thread>
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
                                void (^present)(UIImage*, bool)) {
    if (!renderer.hasDisplayReadback()) return {};
    return std::jthread([&renderer, captureContent, present](std::stop_token stop) mutable {
        std::vector<uint8_t> pixels(size_t(guestWidth) * guestHeight * 4);
        const auto slot = dispatch_semaphore_create(1);
        uint64_t lastFrame = 0;
        while (!stop.stop_requested()) {
            @autoreleasepool {
                if (dispatch_semaphore_wait(slot, DISPATCH_TIME_NOW) == 0) {
                    uint32_t dimensions[2]{};
                    const auto frame = renderer.readDisplay(lastFrame, pixels, dimensions);
                    UIImage* image = frame ? GuestDisplayImage(pixels, dimensions[0], dimensions[1]) : nil;
                    if (image) {
                        lastFrame = frame;
                        const auto bytes = size_t(dimensions[0]) * dimensions[1] * 4;
                        const bool content = captureContent &&
                            nestvane::guestDisplayHasColorVariation(std::span(pixels).first(bytes));
                        if (content) captureContent = false;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            present(image, content);
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

@interface ValidationViewController : UIViewController
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* details;
@property(nonatomic, strong) GuestDisplayView* display;
@property(nonatomic, strong) UIButton* run;
@end

@implementation ValidationViewController

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
    [self.run addTarget:self action:@selector(runValidation) forControlEvents:UIControlEventTouchUpInside];

    UIStackView* layout = [[UIStackView alloc] initWithArrangedSubviews:@[title, self.status, self.details, self.display, self.run]];
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

- (void)runValidation {
    self.run.enabled = NO;
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
            const auto progress = [&](std::string_view event) {
                const auto elapsedSeconds = NSProcessInfo.processInfo.systemUptime - bootStarted;
                milestones += "[" + std::to_string(static_cast<long long>(elapsedSeconds * 1000)) + " ms] ";
                milestones.append(event);
                milestones += '\n';
                constexpr size_t maxMilestoneBytes = 16 * 1024;
                if (milestones.size() > maxMilestoneBytes)
                    milestones.erase(0, milestones.find('\n', milestones.size() - maxMilestoneBytes) + 1);
                exportReport(progressURL, milestones);
            };
            const auto runAndroid = [&]() -> nestvane::DynamicReport {
                if (!ready) {
                    NSLog(@"Unable to create guest data directory: %@", directoryError);
                    return {false, "Cannot create guest data directory\n"};
                }

                NSString* rendererPath = [NSBundle.mainBundle.privateFrameworksPath
                    stringByAppendingPathComponent:@"NestvaneGfxstream.framework/NestvaneGfxstream"];
                if (![manager fileExistsAtPath:rendererPath]) {
                    return nestvane::probeInit(initRoot.fileSystemRepresentation, database.fileSystemRepresentation, progress);
                }

                progress("Initializing local Android renderer");
                nestvane::GfxstreamSession renderer(rendererPath.fileSystemRepresentation, guestWidth, guestHeight);
                if (!renderer) return {false, renderer.error() + "\n"};
                progress("Local Android renderer initialized");
                NSURL* displayProgress = [documents URLByAppendingPathComponent:@"display-first-frame.txt"];
                const BOOL captureFirstFrame = [NSProcessInfo.processInfo.environment[@"NESTVANE_CAPTURE_FIRST_FRAME"]
                    isEqualToString:@"1"];
                auto displayReader = StartDisplayReader(renderer, captureFirstFrame, ^(UIImage* frame, bool content) {
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
                    if (captureFirstFrame && (firstFrame || content)) {
                        NSData* png = UIImagePNGRepresentation(frame);
                        const auto save = [&](NSString* filename) {
                            NSURL* capture = [documents URLByAppendingPathComponent:filename];
                            NSError* error = nil;
                            if (!png || ![png writeToURL:capture options:NSDataWritingAtomic error:&error])
                                NSLog(@"Unable to export %@: %@", filename, error);
                        };
                        if (firstFrame) save(@"display-first-frame.png");
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
                                                progress, renderer.pipeFactory(), touch);
                boot.report.insert(0, "Local Android renderer initialized\n");
                return boot;
            };
            auto probe = runAndroid();
            result.report += "\n" + probe.report;
            if (!probe.passed) status = @"Android 17 startup probe stopped";
            else if (probe.report.find("linkerconfig generation verified: yes") != std::string::npos)
                status = @"Android 17 linker configuration generated";
            else if (probe.report.find("Runtime APEX activation verified: yes") != std::string::npos)
                status = @"Android 17 Runtime APEX activated";
            else status = @"Android 17 init and properties verified";
        }
        if (artStatus) status = artStatus;
        NSString* report = exportReport(outputURL, result.report);

        dispatch_async(dispatch_get_main_queue(), ^{
            self.status.text = status;
            [self.display setTouchDevice:std::shared_ptr<nestvane::GuestTouch>{}];
            self.details.text = report;
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
