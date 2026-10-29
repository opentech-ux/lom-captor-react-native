#import <Foundation/Foundation.h>

@class RCTSurfacePresenter;

NS_ASSUME_NONNULL_BEGIN

typedef void (^SessionRecorderScrollEventSink)(NSString *name, NSDictionary<NSString *, id> *event);

// Private observer activated by the companion's token-scoped startListening.
@interface SessionRecorderScrollObserver : NSObject
- (instancetype)initWithSurfacePresenter:(RCTSurfacePresenter *)presenter
                               eventSink:(SessionRecorderScrollEventSink)eventSink;
- (void)startScrollObservation:(NSString *)activationToken;
- (void)stopScrollObservation:(NSString *)activationToken;
- (void)snapshotScrollSessions:(NSArray *)requests completion:(void (^)(NSArray *results))completion;
- (void)invalidate;
@end

NS_ASSUME_NONNULL_END
