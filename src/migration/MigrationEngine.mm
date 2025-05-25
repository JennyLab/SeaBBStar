// ───── src/MigrationEngine.mm  (v0.2 – NSProgress + os_log/signpost) ──────────
//
//  Platform-specific bridge between the C++ CoreCopy engine and macOS
//  Foundation / unified-logging APIs.
//
//  ✓ Emits an os_log signposted interval (“Migration”) so performance can be
//    inspected in Console.app or Instruments.
//  ✓ Publishes an NSProgress object for real-time completion tracking.
//
//  Build: linked automatically in CMakeLists.txt when APPLE is true.
// ──────────────────────────────────────────────────────────────────────────────
#import <Foundation/Foundation.h>
#import <os/log.h>
#import <os/signpost.h>

#include "core_copy.h"     // C++ CoreCopy

namespace mm {

bool migrate(const std::string& src, const std::string& dst)
{
    /*────────── 1. structured log + interval signpost ─────────*/
    os_log_t logh = os_log_create("com.demo.mini-migration", "engine");
    os_signpost_id_t spid = os_signpost_id_generate(logh);
    os_signpost_interval_begin(logh, spid, "Migration",
                               "src=%{public}s dst=%{public}s",
                               src.c_str(), dst.c_str());

    /*────────── 2. sanity checks ─────────*/
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *srcPath = [NSString stringWithUTF8String:src.c_str()];
    NSString *dstPath = [NSString stringWithUTF8String:dst.c_str()];

    if (![fm fileExistsAtPath:srcPath]) {
        os_log_error(logh, "source file missing");
        os_signpost_interval_end(logh, spid, "Migration", "error=src missing");
        return false;
    }

    uint64_t fileSize =
        [[fm attributesOfItemAtPath:srcPath error:nil] fileSize];

    /*────────── 3. NSProgress publication ─────────*/
    NSProgress *progress =
        [NSProgress progressWithTotalUnitCount:fileSize];
    progress.kind        = NSProgressKindFile;
    progress.pausable    = NO;
    progress.cancellable = NO;
    progress.completedUnitCount = 0;               // start at 0

    /*────────── 4. perform the copy via CoreCopy ─────────*/
    CopyStats st;
    bool ok = CoreCopy::copyResumable(src, dst, st);

    progress.completedUnitCount = fileSize;        // ensure 100 % visible

    /*────────── 5. finish logging ─────────*/
    os_signpost_interval_end(logh, spid, "Migration",
                             "bytes=%{public}llu ok=%{public}d",
                             st.bytesCopied, ok);
    return ok;
}

} // namespace mm
// ───────────────────────────────────────────────────────────────────────────────
