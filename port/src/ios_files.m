#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "ios_files.h"

static NSString *const FOLDER_KEY = @"SaveFolder";
static NSString *const DISC_KEY = @"DiscFile";
static NSString *const FOLDER_NAME = @"DC2";
static NSURL *picked;
static int picking;

@interface PortPicker : NSObject <UIDocumentPickerDelegate>
@end

@implementation PortPicker
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

static NSURL *documents(void) {
    return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
}

static NSURL *remembered(NSString *key) {
    NSData *bookmark = [NSUserDefaults.standardUserDefaults dataForKey:key];
    if (!bookmark) return nil;
    BOOL stale = NO;
    NSURL *item = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
    if (!item || ![item startAccessingSecurityScopedResource]) return nil;
    if (stale) {
        NSData *fresh = [item bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if (fresh) [NSUserDefaults.standardUserDefaults setObject:fresh forKey:key];
    }
    return item;
}

static NSURL *pick(SDL_Window *shown, UTType *type, NSString *key) {
    UIWindow *window = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(shown),
                                                                    SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (!window.rootViewController) return nil;
    PortPicker *delegate = [PortPicker new];
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[type]];
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
    if (bookmark) [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:key];
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

static void download_file(NSURL *file) {
    [NSFileManager.defaultManager startDownloadingUbiquitousItemAtURL:file error:nil];
    while (downloading(file)) {
        int waiting = 1;
        wait_while(&waiting, 0.25);
    }
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
    NSURL *local = [documents() URLByAppendingPathComponent:@"saves" isDirectory:YES];
    if ([files fileExistsAtPath:local.path]) [files copyItemAtURL:local toURL:saves error:nil];
}

static NSURL *documents_disc(void) {
    NSArray<NSURL *> *items = [NSFileManager.defaultManager contentsOfDirectoryAtURL:documents() includingPropertiesForKeys:nil
                                                                              options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                                error:nil];
    for (NSURL *item in [items sortedArrayUsingComparator:^(NSURL *a, NSURL *b) { return [a.path compare:b.path]; }])
        if ([item.pathExtension caseInsensitiveCompare:@"iso"] == NSOrderedSame) return item;
    return nil;
}

static void copy_path(NSURL *item, char *path, size_t size) {
    snprintf(path, size, "%s", item ? item.fileSystemRepresentation : "");
}

const char *port_ios_documents(void) {
    static char path[1024];
    @autoreleasepool {
        if (!path[0]) snprintf(path, sizeof path, "%s/", documents().fileSystemRepresentation);
    }
    return path;
}

void port_ios_disc(char *path, size_t size) {
    @autoreleasepool {
        NSURL *disc = remembered(DISC_KEY);
        copy_path(disc ? disc : documents_disc(), path, size);
    }
}

void port_ios_pick_disc(SDL_Window *window, char *path, size_t size) {
    @autoreleasepool {
        NSURL *disc = pick(window, UTTypeData, DISC_KEY);
        if (!disc) return;
        download_file(disc);
        copy_path(disc, path, size);
    }
}

static int same_folder(NSURL *a, NSURL *b) {
    return [a.URLByStandardizingPath.URLByResolvingSymlinksInPath.path
        isEqualToString:b.URLByStandardizingPath.URLByResolvingSymlinksInPath.path];
}

static NSURL *chosen_saves(void) {
    NSURL *chosen = remembered(FOLDER_KEY);
    return chosen && !same_folder(chosen, documents()) ? chosen : nil;
}

void port_ios_saves_name(char *name, size_t size) {
    @autoreleasepool {
        NSURL *chosen = chosen_saves();
        NSString *shown = nil;
        if (chosen) [chosen getResourceValue:&shown forKey:NSURLLocalizedNameKey error:nil];
        snprintf(name, size, "%s", chosen ? (shown ? shown : chosen.lastPathComponent).UTF8String : "On this device");
    }
}

void port_ios_pick_saves(SDL_Window *window) {
    @autoreleasepool {
        NSURL *chosen = pick(window, UTTypeFolder, FOLDER_KEY);
        if (chosen && same_folder(chosen, documents())) [NSUserDefaults.standardUserDefaults removeObjectForKey:FOLDER_KEY];
    }
}

void port_ios_saves(void) {
    @autoreleasepool {
        NSURL *chosen = chosen_saves();
        NSURL *saves = [chosen ? game_folder(chosen) : documents() URLByAppendingPathComponent:@"saves" isDirectory:YES];
        if (chosen) {
            bring_local_saves(saves);
            download_saves(saves);
        }
        [NSFileManager.defaultManager createDirectoryAtURL:saves withIntermediateDirectories:YES attributes:nil error:nil];
        SDL_setenv_unsafe("DC2_SAVE_ROOT", saves.fileSystemRepresentation, 1);
    }
}
