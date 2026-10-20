#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include "render.h"

static NSString *const FOLDER_KEY = @"SaveFolder";
static NSString *const FOLDER_NAME = @"Dark Chronicle";
static NSURL *picked;
static int picking;

@interface PortFolderPicker : NSObject <UIDocumentPickerDelegate>
@end

@implementation PortFolderPicker
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    picked = urls.firstObject;
    picking = 0;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    picking = 0;
}
@end

static void wait_while(int *flag, double seconds) {
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (*flag && [until timeIntervalSinceNow] > 0) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
}

static NSURL *remembered_folder(void) {
    NSData *bookmark = [NSUserDefaults.standardUserDefaults dataForKey:FOLDER_KEY];
    if (!bookmark) return nil;
    BOOL stale = NO;
    NSURL *folder = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
    if (!folder || ![folder startAccessingSecurityScopedResource]) return nil;
    if (stale) {
        NSData *fresh = [folder bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if (fresh) [NSUserDefaults.standardUserDefaults setObject:fresh forKey:FOLDER_KEY];
    }
    return folder;
}

static NSURL *pick_folder(void) {
    UIWindow *window = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(render_window()),
                                                                    SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (!window.rootViewController) return nil;
    PortFolderPicker *delegate = [PortFolderPicker new];
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder]];
    picker.delegate = delegate;
    picked = nil;
    picking = 1;
    for (int attempt = 0; attempt < 40 && !picker.presentingViewController; ++attempt) {
        int settling = 1;
        wait_while(&settling, 0.25);
        if (!window.rootViewController.presentedViewController)
            [window.rootViewController presentViewController:picker animated:YES completion:nil];
    }
    if (!picker.presentingViewController) return nil;
    wait_while(&picking, 3600);
    if (!picked || ![picked startAccessingSecurityScopedResource]) return nil;
    NSData *bookmark = [picked bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (bookmark) [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:FOLDER_KEY];
    return picked;
}

static NSURL *game_folder(NSURL *chosen) {
    if ([chosen.lastPathComponent isEqualToString:FOLDER_NAME]) return chosen;
    NSURL *folder = [chosen URLByAppendingPathComponent:FOLDER_NAME isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
    return folder;
}

static NSURL *downloaded_name(NSURL *item) {
    NSString *name = item.lastPathComponent;
    if (![name hasPrefix:@"."] || ![name hasSuffix:@".icloud"]) return item;
    NSString *real = [name substringWithRange:NSMakeRange(1, name.length - 8)];
    return [item.URLByDeletingLastPathComponent URLByAppendingPathComponent:real];
}

static int downloading(NSURL *item) {
    [item removeAllCachedResourceValues];
    NSString *status = nil;
    [item getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
    return status && ![status isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent];
}

static NSArray<NSURL *> *pending_files(NSURL *saves) {
    NSMutableArray<NSURL *> *pending = [NSMutableArray array];
    NSDirectoryEnumerator *walk = [NSFileManager.defaultManager enumeratorAtURL:saves includingPropertiesForKeys:nil
                                                                         options:0 errorHandler:nil];
    for (NSURL *item in walk) {
        NSURL *real = downloaded_name(item);
        if (real != item || downloading(real)) [pending addObject:real];
    }
    return pending;
}

static void download_saves(NSURL *saves) {
    for (int round = 0; round < 300; ++round) {
        NSArray<NSURL *> *pending = pending_files(saves);
        if (!pending.count) return;
        for (NSURL *item in pending) [NSFileManager.defaultManager startDownloadingUbiquitousItemAtURL:item error:nil];
        int waiting = 1;
        wait_while(&waiting, 0.1);
    }
}

static void bring_local_saves(NSURL *saves) {
    NSFileManager *files = NSFileManager.defaultManager;
    if ([files fileExistsAtPath:saves.path]) return;
    NSURL *documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *local = [documents URLByAppendingPathComponent:@"saves" isDirectory:YES];
    if ([files fileExistsAtPath:local.path]) [files copyItemAtURL:local toURL:saves error:nil];
}

void port_ios_saves(void) {
    @autoreleasepool {
        NSURL *chosen = remembered_folder();
        if (!chosen) chosen = pick_folder();
        if (!chosen) return;
        NSURL *saves = [game_folder(chosen) URLByAppendingPathComponent:@"saves" isDirectory:YES];
        bring_local_saves(saves);
        download_saves(saves);
        setenv("DC2_SAVE_ROOT", saves.fileSystemRepresentation, 1);
    }
}
