#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include "IOSCamera.hpp"

#include <chrono>
#include <mutex>

using Camera = nestvane::GuestCameraSource;
using Status = Camera::Status;
using Facing = Camera::Facing;

@interface NestvaneCameraCapture : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (BOOL)available:(Facing)facing;
- (BOOL)start:(Facing)facing;
- (Status)status;
- (void)stop;
- (BOOL)copyFrame:(Camera::Format)format into:(std::span<uint8_t>)destination;
@end

@implementation NestvaneCameraCapture {
    AVCaptureDevice* _back;
    AVCaptureDevice* _front;
    AVCaptureDevice* _device; // The one being used.
    AVCaptureSession* _session;
    dispatch_queue_t _queue;
    uint64_t _sessionGeneration;

    std::mutex _lock;
    uint64_t _generation;
    bool _leased;
    Status _status;
    CVPixelBufferRef _frame;
    std::chrono::steady_clock::time_point _lastFrame;
}

- (instancetype)init {
    if (!(self = [super init])) return nil;
    _back = [AVCaptureDevice defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                                              mediaType:AVMediaTypeVideo
                                               position:AVCaptureDevicePositionBack];
    _front = [AVCaptureDevice defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                                               mediaType:AVMediaTypeVideo
                                                position:AVCaptureDevicePositionFront];
    _queue = dispatch_queue_create("dev.nestvane.camera", DISPATCH_QUEUE_SERIAL);
    _status = Status::unavailable;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(appBackgrounded:)
        name:UIApplicationDidEnterBackgroundNotification object:nil];
    return self;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    if (_frame) CVPixelBufferRelease(_frame);
}

- (BOOL)available:(Facing)facing { return (facing == Facing::front ? _front : _back) != nil; }

- (BOOL)ownsGeneration:(uint64_t)generation {
    std::lock_guard lock(_lock);
    return _leased && _generation == generation;
}

- (BOOL)start:(Facing)facing {
    AVCaptureDevice* device = facing == Facing::front ? _front : _back;
    if (!device) return NO;
    uint64_t generation;
    {
        std::lock_guard lock(_lock);
        if (_leased) return NO;
        _device = device;
        _leased = true;
        _status = Status::starting;
        generation = ++_generation;
    }
    dispatch_async(_queue, ^{
        if (![self ownsGeneration:generation]) return;
        const auto access = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        NSLog(@"Nestvane camera: %@ camera asked for, access %ld", self->_device == self->_front ? @"front" : @"back", long(access));
        if (access == AVAuthorizationStatusNotDetermined) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (![self ownsGeneration:generation]) return;
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
                    dispatch_async(self->_queue, ^{ [self open:generation authorized:granted]; });
                }];
            });
        } else {
            [self open:generation authorized:access == AVAuthorizationStatusAuthorized];
        }
    });
    return YES;
}

- (void)open:(uint64_t)generation authorized:(BOOL)authorized {
    {
        std::lock_guard lock(_lock);
        if (_generation != generation || !_leased) return;
        if (!authorized) {
            _status = Status::unavailable;
            return;
        }
    }
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:_device error:&error];
    AVCaptureVideoDataOutput* output = [AVCaptureVideoDataOutput new];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{(NSString*)kCVPixelBufferPixelFormatTypeKey:
        @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    AVCaptureSession* session = [AVCaptureSession new];
    const auto unavailable = [&] {
        std::lock_guard lock(_lock);
        if (_generation == generation) _status = Status::unavailable;
    };
    if (!input || ![session canSetSessionPreset:AVCaptureSessionPreset640x480]) {
        unavailable();
        return;
    }
    session.sessionPreset = AVCaptureSessionPreset640x480;
    if (![session canAddInput:input] || ![session canAddOutput:output]) {
        unavailable();
        return;
    }
    [session addInput:input];
    [session addOutput:output];
    AVCaptureConnection* connection = [output connectionWithMediaType:AVMediaTypeVideo];
    if ([connection isVideoRotationAngleSupported:0]) connection.videoRotationAngle = 0;
    // The sensor's own picture, unmirrored, as an Android camera gives it: the app mirrors a front preview itself.
    if (connection.isVideoMirroringSupported) {
        connection.automaticallyAdjustsVideoMirroring = NO;
        connection.videoMirrored = NO;
    }
    [output setSampleBufferDelegate:self queue:_queue];
    if (![self ownsGeneration:generation]) return;
    _session = session;
    _sessionGeneration = generation;
    [session startRunning];
    NSLog(@"Nestvane camera: session running %d", session.running);
    if (!session.running) unavailable();
}

- (Status)status {
    std::lock_guard lock(_lock);
    return _status;
}

- (void)invalidate:(BOOL)release {
    uint64_t generation;
    {
        std::lock_guard lock(_lock);
        _status = Status::unavailable;
        if (release) _leased = false;
        generation = ++_generation;
        if (_frame) CVPixelBufferRelease(_frame);
        _frame = nullptr;
    }
    dispatch_async(_queue, ^{
        if (self->_sessionGeneration < generation) {
            [self->_session stopRunning];
            self->_session = nil;
        }
    });
}

- (void)stop { [self invalidate:YES]; }

- (void)appBackgrounded:(NSNotification*)notification {
    (void)notification;
    [self invalidate:NO];
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    CVPixelBufferRef frame = CMSampleBufferGetImageBuffer(sample);
    if (!frame) return;
    std::lock_guard lock(_lock);
    if (!_leased || _generation != _sessionGeneration) return;
    if (CVPixelBufferGetWidth(frame) != Camera::width || CVPixelBufferGetHeight(frame) != Camera::height ||
        CVPixelBufferGetPixelFormatType(frame) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
        _status = Status::unavailable;
        return;
    }
    if (_frame) CVPixelBufferRelease(_frame);
    _frame = CVPixelBufferRetain(frame);
    _lastFrame = std::chrono::steady_clock::now();
    _status = Status::ready;
}

- (BOOL)copyFrame:(Camera::Format)format into:(std::span<uint8_t>)destination {
    std::lock_guard lock(_lock);
    if (!_frame || _status != Status::ready || destination.size() != Camera::frameBytes(format) ||
        std::chrono::steady_clock::now() - _lastFrame > std::chrono::seconds(2)) return NO;
    const CVPixelBufferRef buffer = _frame;
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return NO;
    const auto* yPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    const auto* uvPlane = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    const auto yStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    const auto uvStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    constexpr auto width = Camera::width, height = Camera::height;
    if (format == Camera::Format::yuv420) {
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(destination.data() + y * width, yPlane + y * yStride, width);
        const auto uStart = width * height, vStart = uStart + width * height / 4;
        for (unsigned y = 0; y < height / 2; ++y) {
            for (unsigned x = 0; x < width / 2; ++x) {
                const auto offset = y * width / 2 + x;
                destination[uStart + offset] = uvPlane[y * uvStride + x * 2];
                destination[vStart + offset] = uvPlane[y * uvStride + x * 2 + 1];
            }
        }
    } else {
        const auto clip = [](int value) { return uint8_t(std::clamp(value / 256, 0, 255)); };
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto luma = 298 * (int(yPlane[y * yStride + x]) - 16);
                const auto chroma = y / 2 * uvStride + (x & ~1U);
                const auto u = int(uvPlane[chroma]) - 128, v = int(uvPlane[chroma + 1]) - 128;
                const auto index = (y * width + x) * 4;
                destination[index] = clip(luma + 409 * v + 128);
                destination[index + 1] = clip(luma - 100 * u - 208 * v + 128);
                destination[index + 2] = clip(luma + 516 * u + 128);
                destination[index + 3] = 255;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return YES;
}
@end

namespace nestvane {
namespace {
class IOSCameraSource final : public GuestCameraSource {
public:
    bool available(Facing facing) const override { return [capture available:facing]; }
    bool start(Facing facing) override { return [capture start:facing]; }
    Status status() const override { return [capture status]; }
    bool copyFrame(Format format, std::span<uint8_t> target) override { return [capture copyFrame:format into:target]; }
    void stop() override { [capture stop]; }
private:
    NestvaneCameraCapture* capture = [NestvaneCameraCapture new];
};
} // namespace

std::shared_ptr<GuestCameraSource> makeIOSCamera() {
    return std::make_shared<IOSCameraSource>();
}
} // namespace nestvane
