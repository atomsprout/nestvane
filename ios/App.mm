#import <UIKit/UIKit.h>

#include "DynamicProbe.hpp"
#include "Validation.hpp"

static NSString* ThermalState() {
    switch (NSProcessInfo.processInfo.thermalState) {
        case NSProcessInfoThermalStateNominal: return @"nominal";
        case NSProcessInfoThermalStateFair: return @"fair";
        case NSProcessInfoThermalStateSerious: return @"serious";
        case NSProcessInfoThermalStateCritical: return @"critical";
    }
    return @"unknown";
}

@interface ValidationViewController : UIViewController
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* details;
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

    self.run = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.run setTitle:@"Run validation" forState:UIControlStateNormal];
    [self.run addTarget:self action:@selector(runValidation) forControlEvents:UIControlEventTouchUpInside];

    UIStackView* layout = [[UIStackView alloc] initWithArrangedSubviews:@[title, self.status, self.details, self.run]];
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
            auto probe = ready ? nestvane::probeInit(initRoot.fileSystemRepresentation, database.fileSystemRepresentation, progress) :
                nestvane::DynamicReport{false, "Cannot create guest data directory\n"};
            if (!ready) NSLog(@"Unable to create guest data directory: %@", directoryError);
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
