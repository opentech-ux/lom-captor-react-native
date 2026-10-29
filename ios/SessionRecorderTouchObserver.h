#import <Foundation/Foundation.h>

#include "SessionRecorderObserver.h"

@class RCTSurfacePresenter;

NS_ASSUME_NONNULL_BEGIN

typedef void (^SessionRecorderTouchEventSink)(NSDictionary<NSString *, id> *event);

// Private observer activated by the companion's token-scoped startListening.
@interface SessionRecorderTouchObserver : NSObject
- (instancetype)initWithSurfacePresenter:(RCTSurfacePresenter *)presenter
                               eventSink:(SessionRecorderTouchEventSink)eventSink;
- (void)startTouchObservation:(NSString *)activationToken;
- (void)stopTouchObservation:(NSString *)activationToken;
- (void)invalidate;
// Arms/retires recovery on the existing native contact, without a second registry.
- (BOOL)withActiveTouch:(NSUInteger)pointerId activation:(NSString *)activation
               surface:(NSInteger)surface instance:(const std::string &)instance
     captureGeneration:(uint64_t)captureGeneration awaitingLom:(BOOL)awaitingLom perform:(void (^)(void))perform;
// Thread-safe, read-only clock sample; no UIKit or main-thread dispatch.
- (lomtree::ScanClockSample)scanClockSample;
- (std::optional<lomtree::PointerDownRecovery>)recoveryForSurface:(NSInteger)surface;
- (void)finishRecovery:(const lomtree::PointerDownRecovery &)recovery;
@end

NS_ASSUME_NONNULL_END
