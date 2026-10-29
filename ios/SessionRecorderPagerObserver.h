#import <Foundation/Foundation.h>
#include "SessionRecorderPagerRegistry.h"
@class RCTSurfacePresenter;

@interface SessionRecorderPagerObserver : NSObject
- (instancetype)initWithPresenter:(RCTSurfacePresenter *)presenter
                         registry:(std::shared_ptr<lomtree::SessionRecorderPagerRegistry>)registry
                             sink:(void (^)(NSDictionary *))sink;
- (void)start:(NSString *)activation;
- (void)stop;
@end
