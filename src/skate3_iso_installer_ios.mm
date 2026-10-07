// iOS counterparts of the macOS NSOpenPanel pickers and the curl-based title
// update download. Kept under the same function names so the installers need
// no iOS-specific call sites.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface Skate3DocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, strong) NSURL* pickedURL;
@property(nonatomic, assign) BOOL finished;
@end

@implementation Skate3DocumentPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  self.pickedURL = urls.firstObject;
  self.finished = YES;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  self.finished = YES;
}
@end

namespace skate3 {
namespace {

UIViewController* TopViewController() {
  UIViewController* root = nil;
  SDL_Window* sdl_window = SDL_GetKeyboardFocus();
  if (sdl_window) {
    SDL_PropertiesID properties = SDL_GetWindowProperties(sdl_window);
    UIWindow* window = (__bridge UIWindow*)SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    root = window.rootViewController;
  }
  if (!root) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:[UIWindowScene class]]) {
        continue;
      }
      for (UIWindow* window in ((UIWindowScene*)scene).windows) {
        if (window.isKeyWindow) {
          root = window.rootViewController;
          break;
        }
      }
    }
  }
  while (root.presentedViewController) {
    root = root.presentedViewController;
  }
  return root;
}

// Presents a document picker and spins the main run loop until the user picks
// or cancels. The installers call their pickers synchronously from the UI
// thread (the same contract as NSOpenPanel's runModal), so the nested run loop
// keeps UIKit responsive while the wizard waits.
//
// The file is opened in place rather than copied: a copy of a ~7 GB ISO would
// double the storage it needs. Security-scoped access is started and
// intentionally never stopped, because extraction reads the file on a worker
// thread after this returns; the grant ends with the process.
std::filesystem::path PickDocument(NSArray<UTType*>* types) {
  @autoreleasepool {
    UIViewController* presenter = TopViewController();
    if (!presenter) {
      return {};
    }
    Skate3DocumentPickerDelegate* delegate = [[Skate3DocumentPickerDelegate alloc] init];
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
    picker.delegate = delegate;
    picker.allowsMultipleSelection = NO;
    picker.shouldShowFileExtensions = YES;
    picker.directoryURL = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                               inDomains:NSUserDomainMask]
                              .firstObject;
    [presenter presentViewController:picker animated:YES completion:nil];

    while (!delegate.finished) {
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }

    NSURL* url = delegate.pickedURL;
    if (!url || !url.isFileURL) {
      return {};
    }
    [url startAccessingSecurityScopedResource];
    return std::string(url.path.fileSystemRepresentation);
  }
}

}  // namespace

std::filesystem::path PickIsoFileMacOS() {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  UTType* iso_type = [UTType typeWithFilenameExtension:@"iso"];
  if (iso_type) {
    [types addObject:iso_type];
  }
  // Disk images are often tagged as generic data by the Files provider.
  [types addObject:UTTypeData];
  return PickDocument(types);
}

std::filesystem::path PickTitleUpdateFileMacOS() {
  // The TU package (TU_12K2276_000000C000000.00000000000O3) has no extension.
  return PickDocument(@[ UTTypeData, UTTypeItem ]);
}

bool DownloadToFileApple(const std::string& url_string, const std::filesystem::path& destination,
                         std::atomic<uint64_t>& copied_bytes, std::atomic<uint64_t>& total_bytes,
                         std::string& error) {
  @autoreleasepool {
    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:url_string.c_str()]];
    if (!url) {
      error = "The configured title update URL is invalid.";
      return false;
    }
    NSURLSessionConfiguration* config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest = 30;
    config.timeoutIntervalForResource = 600;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:config];

    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSURL* downloaded = nil;
    __block NSString* failure = nil;
    __block NSInteger status = 0;
    NSURLSessionDownloadTask* task =
        [session downloadTaskWithURL:url
                   completionHandler:^(NSURL* location, NSURLResponse* response, NSError* err) {
                     if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                       status = ((NSHTTPURLResponse*)response).statusCode;
                     }
                     if (err) {
                       failure = err.localizedDescription;
                     } else if (location) {
                       // The temporary file is deleted when this handler returns.
                       NSURL* kept = [NSURL
                           fileURLWithPath:[NSString stringWithUTF8String:destination.c_str()]];
                       [NSFileManager.defaultManager removeItemAtURL:kept error:nil];
                       NSError* move_error = nil;
                       if ([NSFileManager.defaultManager moveItemAtURL:location
                                                                 toURL:kept
                                                                 error:&move_error]) {
                         downloaded = kept;
                       } else {
                         failure = move_error.localizedDescription;
                       }
                     }
                     dispatch_semaphore_signal(done);
                   }];
    [task resume];
    while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC))) {
      const int64_t expected = task.countOfBytesExpectedToReceive;
      if (expected > 0) {
        total_bytes.store(uint64_t(expected), std::memory_order_relaxed);
      }
      copied_bytes.store(uint64_t(task.countOfBytesReceived), std::memory_order_relaxed);
    }
    [session finishTasksAndInvalidate];

    if (!downloaded || (status != 0 && (status < 200 || status >= 300))) {
      std::string detail = failure ? std::string(failure.UTF8String)
                                   : ("HTTP status " + std::to_string(status));
      error = "The download failed (" + detail +
              "). Check your internet connection, or select the title update file manually.";
      return false;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(destination, ec);
    if (ec || size == 0) {
      error = "The download produced no data.";
      return false;
    }
    total_bytes.store(size, std::memory_order_relaxed);
    copied_bytes.store(size, std::memory_order_relaxed);
    return true;
  }
}

}  // namespace skate3
