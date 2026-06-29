// Offline ER menu-texture extractor (Linux).
//
// Decompresses a menu .tpf.dcx (DCX-KRAK) using the real Linux Oodle lib shipped with the game
// (liboo2corelinux64.so.9) via dlopen, then pulls named DDS textures out of the TPF so we can SEE
// the glyph sheets (SB_MapCursor / SB_MapCursor_ERR / SB_Icon_*) instead of guessing.
//
// Reuses the in-game-validated msbe::dcx_decompress + msbe::tpf_find_texture (compiled in).
// Enumerates texture names by UTF-16LE string scan (TPF entry stride is variable due to the
// optional FloatStruct, so a scan is the robust way to LIST names); extraction itself goes through
// tpf_find_texture, which knows the exact entry layout.
//
// Build: tools/build_menu_tex_extract.sh   Run: ./tools/menu_tex_extract [01_common.tpf.dcx]

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <dlfcn.h>

#include "worldmap/msbe_parser.hpp"   // goblin::msbe::{dcx_decompress, tpf_find_texture, OodleDecompressFn}

using goblin::msbe::dcx_decompress;
using goblin::msbe::tpf_find_texture;
using goblin::msbe::OodleDecompressFn;

static const char *OODLE_LIB =
    "/home/iamacat/Games/ERRv2.2.9.6/internals/launcher/liboo2corelinux64.so.9";
static const char *DEFAULT_DCX =
    "/home/iamacat/Games/ERRv2.2.9.6/mod/menu/hi/01_common.tpf.dcx";
static const char *OUTDIR = "tools/extracted";

static std::vector<uint8_t> slurp(const char *path)
{
    std::vector<uint8_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (std::fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear(); }
    std::fclose(f);
    return v;
}

static bool dump(const std::string &path, const uint8_t *p, size_t n)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(p, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

// Collect UTF-16LE printable strings (>=3 chars) from the blob — gives the TPF texture names.
static std::vector<std::string> scan_utf16_names(const uint8_t *b, size_t n)
{
    std::set<std::string> uniq;
    std::string cur;
    for (size_t i = 0; i + 1 < n; i += 2)
    {
        uint8_t lo = b[i], hi = b[i + 1];
        if (hi == 0 && lo >= 0x20 && lo < 0x7f) { cur.push_back((char)lo); }
        else { if (cur.size() >= 3) uniq.insert(cur); cur.clear(); }
    }
    if (cur.size() >= 3) uniq.insert(cur);
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

int main(int argc, char **argv)
{
    const char *dcxPath = argc > 1 ? argv[1] : DEFAULT_DCX;

    void *h = dlopen(OODLE_LIB, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { std::fprintf(stderr, "[ERR] dlopen %s: %s\n", OODLE_LIB, dlerror()); return 1; }
    auto oodle = (OodleDecompressFn)dlsym(h, "OodleLZ_Decompress");
    if (!oodle) { std::fprintf(stderr, "[ERR] dlsym OodleLZ_Decompress: %s\n", dlerror()); return 1; }
    std::printf("[ok] Oodle bound: %s\n", OODLE_LIB);

    std::vector<uint8_t> dcx = slurp(dcxPath);
    if (dcx.empty()) { std::fprintf(stderr, "[ERR] read %s\n", dcxPath); return 1; }
    bool isKrak = false;
    std::vector<uint8_t> tpf = dcx_decompress(dcx.data(), dcx.size(), &isKrak, oodle);
    std::printf("[ok] dcx %s: in=%zu out=%zu krak=%d\n", dcxPath, dcx.size(), tpf.size(), (int)isKrak);
    if (tpf.empty()) { std::fprintf(stderr, "[ERR] dcx_decompress failed\n"); return 1; }
    if (tpf.size() < 4 || std::memcmp(tpf.data(), "TPF\0", 4) != 0)
        std::fprintf(stderr, "[warn] decompressed blob magic != TPF (%.4s)\n", (const char *)tpf.data());

    std::vector<std::string> names = scan_utf16_names(tpf.data(), tpf.size());
    std::printf("\n=== TPF texture names (UTF-16 scan, %zu strings) ===\n", names.size());
    for (auto &s : names) std::printf("  %s\n", s.c_str());

    std::string mk = std::string("mkdir -p ") + OUTDIR;
    if (std::system(mk.c_str())) {}

    std::printf("\n=== extraction ===\n");
    int got = 0;
    for (auto &nm : names)
    {
        if (nm.rfind("SB_", 0) != 0) continue;   // only the sheet atlases
        size_t off = 0, len = 0;
        if (!tpf_find_texture(tpf.data(), tpf.size(), nm.c_str(), off, len))
        {
            std::printf("  [skip] %-28s (miss / per-entry-DCX / non-raw)\n", nm.c_str());
            continue;
        }
        std::string out = std::string(OUTDIR) + "/" + nm + ".dds";
        if (dump(out, tpf.data() + off, len)) { std::printf("  [dds ] %-28s %zu bytes -> %s\n", nm.c_str(), len, out.c_str()); ++got; }
        else std::printf("  [ERR ] write %s\n", out.c_str());
    }
    std::printf("[ok] extracted %d DDS\n", got);

    // DDS -> PNG via ImageMagick for visual inspection.
    std::printf("\n=== png convert (ImageMagick) ===\n");
    std::string conv = std::string("for f in ") + OUTDIR +
        "/*.dds; do [ -e \"$f\" ] || continue; magick \"$f\" \"${f%.dds}.png\" 2>/dev/null "
        "&& echo \"  [png ] ${f%.dds}.png\" || echo \"  [fail] $f (magick)\"; done";
    if (std::system(conv.c_str())) {}
    std::printf("[done]\n");
    return 0;
}
