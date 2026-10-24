// GOW-Port: iOS Apple-hosted Background Assets (iOS 26+). The game's large disc files come as essential asset packs that
// the system downloads with the app (GoWPad's downloader extension); this resolves a file of those packs to a path the
// runtime can open. Compiled with ARC.
#import <BackgroundAssets/BackgroundAssets.h>
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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
        NSString *name = [NSString stringWithUTF8String:file];
        NSError *urlError = nil;
        NSURL *url = [manager URLForPath:name error:&urlError];
        if (url && url.fileURL)
        {
            // the packs live in the Background Assets daemon's container: the path may not be open to the app
            if (std::FILE *probe = std::fopen(url.path.UTF8String, "rb"))
            {
                std::fclose(probe);
                path = url.path.UTF8String;
                std::fprintf(stderr, "[ios-assets] %s readable at %s\n", file, path.c_str());
                return true;
            }
            std::fprintf(stderr, "[ios-assets] %s not readable by path (%s): %s\n", file, std::strerror(errno), url.path.UTF8String);
        }
        else
            std::fprintf(stderr, "[ios-assets] %s in %s: no URL (%s)\n", file, packId,
                         urlError ? urlError.localizedDescription.UTF8String : "none");
        // the descriptor the system opens for the app; /dev/fd/<n> reopens it for the runtime's file streams
        NSError *fdError = nil;
        const int fd = [manager fileDescriptorForPath:name searchingInAssetPackWithIdentifier:identifier error:&fdError];
        if (fd < 0)
        {
            std::fprintf(stderr, "[ios-assets] %s: no descriptor (%s)\n", file,
                         fdError ? fdError.localizedDescription.UTF8String : "none");
            return false;
        }
        path = "/dev/fd/" + std::to_string(fd);
        std::FILE *probe = std::fopen(path.c_str(), "rb");
        std::fprintf(stderr, "[ios-assets] %s through descriptor %d: %s\n", file, fd, probe ? "readable" : std::strerror(errno));
        if (!probe)
            return false;
        std::fclose(probe);
        return true;
    }
    std::fprintf(stderr, "[ios-assets] asset packs need iOS 26\n");
    return false;
}
