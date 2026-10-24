// GOW-Port: iOS Apple-hosted Background Assets (iOS 26+). The game's large disc files come as essential asset packs that
// the system downloads with the app (GoWPad's downloader extension); this resolves a file of those packs to a path the
// runtime can open. Compiled with ARC.
#import <BackgroundAssets/BackgroundAssets.h>
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <cstdio>
#include <string>

namespace
{
    // waits for a completion handler without blocking forever. This runs on the main thread at start-up: the main run
    // loop keeps turning meanwhile, so handlers the system delivers on the main queue still arrive.
    bool waitFor(dispatch_semaphore_t done, double seconds)
    {
        const CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + seconds;
        while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) != 0)
        {
            if (CFAbsoluteTimeGetCurrent() >= until)
                return false;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
        }
        return true;
    }
}

// The path of `file` inside asset pack `packId`, once the pack is on the device. Starts the pack's download when it is
// missing (essential packs normally arrive with the app) and returns false meanwhile.
bool ps2xIosAssetPackFile(const char *packId, const char *file, std::string &path)
{
#if TARGET_OS_SIMULATOR
    // (Background Assets traps in unsigned simulator builds: "The process lacks a team ID")
    std::fprintf(stderr, "[ios-assets] no asset packs in the simulator\n");
    (void)packId; (void)file; (void)path;
    return false;
#endif
    if (@available(iOS 26.0, *))
    {
        BAAssetPackManager *manager = [BAAssetPackManager sharedManager];
        NSString *identifier = [NSString stringWithUTF8String:packId];
        __block BAAssetPack *pack = nil;
        __block NSError *failure = nil;
        dispatch_semaphore_t got = dispatch_semaphore_create(0);
        [manager getAssetPackWithIdentifier:identifier
                          completionHandler:^(BAAssetPack *assetPack, NSError *error) {
                              pack = assetPack;
                              failure = error;
                              dispatch_semaphore_signal(got);
                          }];
        if (!waitFor(got, 15.0) || !pack)
        {
            std::fprintf(stderr, "[ios-assets] asset pack %s unavailable: %s\n", packId,
                         failure ? failure.localizedDescription.UTF8String : "no answer");
            return false;
        }
        __block BAAssetPackStatus status = 0;
        dispatch_semaphore_t statusDone = dispatch_semaphore_create(0);
        [manager getStatusOfAssetPackWithIdentifier:identifier
                                  completionHandler:^(BAAssetPackStatus value, NSError *) {
                                      status = value;
                                      dispatch_semaphore_signal(statusDone);
                                  }];
        waitFor(statusDone, 10.0);
        std::fprintf(stderr, "[ios-assets] %s version %ld, %ld bytes, status 0x%lx\n", packId, static_cast<long>(pack.version),
                     static_cast<long>(pack.downloadSize), static_cast<unsigned long>(status));
        // ensureLocalAvailability returns at once for a pack on the device; a missing one downloads (minutes for these)
        __block NSError *availability = nil;
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        [manager ensureLocalAvailabilityOfAssetPack:pack
                                  completionHandler:^(NSError *error) {
                                      availability = error;
                                      dispatch_semaphore_signal(ready);
                                  }];
        if (!waitFor(ready, 10.0))
        {
            std::fprintf(stderr, "[ios-assets] %s is still downloading\n", packId);
            return false;
        }
        if (availability)
        {
            std::fprintf(stderr, "[ios-assets] %s: %s\n", packId, availability.localizedDescription.UTF8String);
            return false;
        }
        NSError *urlError = nil;
        NSURL *url = [manager URLForPath:[NSString stringWithUTF8String:file] error:&urlError];
        if (!url || !url.fileURL)
        {
            std::fprintf(stderr, "[ios-assets] %s in %s: %s\n", file, packId,
                         urlError ? urlError.localizedDescription.UTF8String : "no file URL");
            return false;
        }
        path = url.path.UTF8String;
        return true;
    }
    std::fprintf(stderr, "[ios-assets] asset packs need iOS 26\n");
    return false;
}
