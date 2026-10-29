#import <Foundation/Foundation.h>
#import <React/RCTSurfacePresenter.h>
#include "SessionRecorderScreenVisibilityRegistry.h"

@interface SessionRecorderScreenVisibilityObserver : NSObject
@property (nonatomic, copy) void (^onPublished)(NSInteger surface);
- (instancetype)initWithPresenter:(RCTSurfacePresenter *)presenter
                          registry:(std::shared_ptr<lomtree::SessionRecorderScreenVisibilityRegistry>)registry;
- (void)start:(NSString *)activation;
- (void)stop;
@end
