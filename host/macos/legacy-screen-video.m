// SPDX-License-Identifier: GPL-3.0-or-later
// Use Sunshine's AVFoundation capture on Big Sur. No ScreenCaptureKit linkage.
#import "screen-video.h"

@implementation DPScreenVideo
- (BOOL)hdrAvailable { return NO; }
@synthesize hdrCaptureAllowed;
- (dispatch_semaphore_t)capture:(FrameCallbackBlock)callback {
    // The inherited capture path supports SDR BGRA/NV12, not DeskPort's HDR path.
    if (self.pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange) return nil;
    return [super capture:callback];
}
- (BOOL)captureFailed:(dispatch_semaphore_t)signal {
    // Setup errors return nil in AVVideo; completion follows its callback contract.
    (void)signal;
    return NO;
}
@end
