// ───── src/core_copy.cpp  (v0.3 → delta-aware) ──────────────────────────────
#include "../include/core_copy.h"
#include "../include/diff_index.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <system_error>
#include <vector>

#if defined(OPENSSL_FOUND)
  #include <openssl/evp.h>
#else
  #ifdef __APPLE__
    #include <CommonCrypto/CommonDigest.h>
  #endif
#endif

namespace mm {
namespace fs = std::filesystem;

/* ───── helpers for .resume.meta ─────────────────────────────────────────── */
namespace {
constexpr const char* META_EXT = ".resume.meta";

uint64_t read_meta(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    uint64_t off = 0;
    if (in) in.read(reinterpret_cast<char*>(&off), sizeof(off));
    return off;
}
void write_meta(const fs::path& p, uint64_t off) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&off), sizeof(off));
}
void remove_meta(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
}
} // anonymous namespace

/* ───── resumable copy with optional delta mode ──────────────────────────── */
bool CoreCopy::copyResumable(const fs::path& src,
                             const fs::path& dst,
                             CopyStats&      st,
                             size_t          initialChunk,
                             const CopyOptions* opts)
{
    try {
        if (!fs::exists(src)) {
            std::cerr << "src not exist\n";
            return false;
        }

        const bool delta = opts && opts->deltaMode;

        /*──────────── delta-only fast path ───────────────────────────────*/
        if (delta && fs::exists(dst)) {
            fs::path tmp = dst;
            tmp += (opts ? opts->tmpSuffix : ".patching");

            fs::copy_file(dst, tmp, fs::copy_options::overwrite_existing);

            auto oldIdx = DiffIndex::build(tmp);
            auto newIdx = DiffIndex::build(src);
            auto ranges = DiffIndex::diff(oldIdx, newIdx);

            std::fstream out(tmp, std::ios::in | std::ios::out | std::ios::binary);
            for (auto [beg, end] : ranges)
                copyRange(src, out, beg, end, initialChunk);

            out.close();

            std::error_code ec;
            fs::remove(dst, ec);          // remove existing file, ignore error
            fs::rename(tmp, dst);         // atomic replace

            st.bytesCopied = fs::file_size(dst);
            return true;                  // delta path done
        }
        /*──────── fall through: full / resumable copy ───────────────────*/

        fs::path meta = dst;
        meta += META_EXT;
        uint64_t offset = fs::exists(meta) ? read_meta(meta) : 0;

        std::ifstream in(src, std::ios::binary);
        std::ofstream out(dst,
                          std::ios::binary | std::ios::in | std::ios::out |
                              std::ios::ate);
        if (!out) out.open(dst, std::ios::binary);   // first run

        in.seekg(offset);
        out.seekp(offset);
        st.bytesCopied = offset;

        /* adaptive-chunk copy loop */
        size_t   curChunk = initialChunk;                    // 4 MiB start
        constexpr uint64_t WIN = 64 * 1024 * 1024;           // 64 MiB window
        uint64_t bytesWin = 0;
        auto tWinStart = std::chrono::steady_clock::now();
        std::vector<char> buf(curChunk);

        while (in) {
            in.read(buf.data(), curChunk);
            std::streamsize got = in.gcount();
            if (got <= 0) break;

            out.write(buf.data(), got);
            offset        += got;
            st.bytesCopied = offset;
            bytesWin      += got;

            write_meta(meta, offset);
            out.flush();   // durability (replace with fdatasync for perf)

            if (bytesWin >= WIN) {        // adapt chunk size
                auto   now  = std::chrono::steady_clock::now();
                double mbps = (bytesWin / 1.0e6) /
                              std::chrono::duration<double>(now - tWinStart)
                                  .count();

                if (mbps < 150.0 && curChunk > 256 * 1024)
                    curChunk /= 2;
                else if (mbps > 450.0 && curChunk < 16 * 1024 * 1024)
                    curChunk *= 2;

                buf.resize(curChunk);
                bytesWin  = 0;
                tWinStart = now;
            }
        }

        out.close();
        in.close();
        remove_meta(meta);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "copy exception: " << e.what() << '\n';
        return false;
    }
}

/* ───── SHA-256 utility (OpenSSL / CommonCrypto) ─────────────────────────── */
std::string CoreCopy::sha256(const fs::path& file)
{
    constexpr size_t BUF = 1 << 16;
    unsigned char hash[32];

#if defined(OPENSSL_FOUND)
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
#else
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
#endif

    std::ifstream in(file, std::ios::binary);
    std::vector<char> buf(BUF);
    while (in) {
        in.read(buf.data(), buf.size());
        std::streamsize n = in.gcount();
        if (n > 0) {
#if defined(OPENSSL_FOUND)
            EVP_DigestUpdate(ctx, buf.data(), n);
#else
            CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(n));
#endif
        }
    }
#if defined(OPENSSL_FOUND)
    EVP_DigestFinal_ex(ctx, hash, nullptr);
    EVP_MD_CTX_free(ctx);
#else
    CC_SHA256_Final(hash, &ctx);
#endif

    static constexpr char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[2 * i]     = hex[(hash[i] >> 4) & 0xF];
        out[2 * i + 1] = hex[hash[i] & 0xF];
    }
    return out;
}

/* ───── copyRange helper for delta mode ───────────────────────────────────── */
void CoreCopy::copyRange(const fs::path& src,
                         std::fstream&   out,
                         uint64_t        beg,
                         uint64_t        end,
                         size_t          chunk)
{
    std::ifstream in(src, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(beg));
    out.seekp(static_cast<std::streamoff>(beg));

    std::vector<char> buf(chunk);
    uint64_t remain = end - beg;
    while (remain) {
        size_t n = std::min<uint64_t>(remain, chunk);
        in.read(buf.data(), n);
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        out.write(buf.data(), got);
        remain -= static_cast<uint64_t>(got);
    }
}

} // namespace mm
// ────────────────────────────────────────────────────────────────────────────
