// ─── src/diff_index.cpp ────────────────────────────────────────────
#include "../include/diff_index.h"

#include <city.h>            // CityHash64
#include <fstream>
#include <vector>

namespace mm {
namespace fs = std::filesystem;

/*----------------------------------------------------------------------*
 |  Build a block-hash index of a file                                  |
 |  Each BlockHash records offset, length, and a 64-bit fast hash.      |
 *----------------------------------------------------------------------*/
std::vector<BlockHash> DiffIndex::build(const fs::path& p, size_t blk)
{
    std::vector<BlockHash> out;
    std::ifstream in(p, std::ios::binary);
    if (!in) return out;                         // file missing? return empty.

    std::vector<char> buf(blk);
    uint64_t off = 0;

    while (in) {
        in.read(buf.data(), blk);
        std::streamsize n = in.gcount();
        if (n <= 0) break;

        uint64_t h = CityHash64(buf.data(), static_cast<size_t>(n));
        out.push_back({off, static_cast<uint64_t>(n), h});
        off += static_cast<uint64_t>(n);
    }
    return out;
}

/*----------------------------------------------------------------------*
 |  Compute byte-range deltas between two indices                       |
 |  Returns vector of <start,end> half-open ranges that differ.         |
 *----------------------------------------------------------------------*/
std::vector<std::pair<uint64_t, uint64_t>>
DiffIndex::diff(const std::vector<BlockHash>& a,
                const std::vector<BlockHash>& b)
{
    std::vector<std::pair<uint64_t, uint64_t>> delta;
    size_t i = 0, j = 0;

    while (i < a.size() && j < b.size()) {
        if (a[i].hash64 == b[j].hash64) {        // unchanged block
            ++i; ++j;
            continue;
        }

        // start of changed run in 'b'
        uint64_t start = b[j].off;
        uint64_t end   = start + b[j].len;
        ++j;

        // extend run until hashes line up again (or one list ends)
        while (j < b.size() && a[i].hash64 != b[j].hash64) {
            end += b[j].len;
            ++j;
        }
        delta.emplace_back(start, end);
    }

    // Tail-case: insertions at end of file
    while (j < b.size()) {
        delta.emplace_back(b[j].off, b.back().off + b.back().len);
        break;            // single range covers the remainder
    }
    return delta;
}

} // namespace mm
