#include <iostream>
#include "core_copy.h"

namespace mm {

// Linux / WSL CoreCopy
bool migrate(const std::string& src, const std::string& dst)
{
    CopyStats st;
    bool ok = CoreCopy::copyResumable(src, dst, st);
    if (!ok)
        std::cerr << "Migration failed on non-mac host\n";
    return ok;
}

} // namespace mm
