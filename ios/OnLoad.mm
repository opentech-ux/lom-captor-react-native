#import <Foundation/Foundation.h>
#import "SessionRecorderReactNativeImpl.h"
#import <ReactCommon/CxxTurboModuleUtils.h>

@interface SessionRecorderReactNativeOnLoad : NSObject
@end

@implementation SessionRecorderReactNativeOnLoad

using namespace facebook::react;

+ (void)load
{
  registerCxxModuleToGlobalModuleMap(
    std::string(SessionRecorderReactNativeImpl::kModuleName),
    [](std::shared_ptr<CallInvoker> jsInvoker) {
      return std::make_shared<SessionRecorderReactNativeImpl>(jsInvoker);
    }
  );
}

@end
