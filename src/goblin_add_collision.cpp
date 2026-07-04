#include "goblin_add_collision.hpp"

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <spdlog/spdlog.h>

namespace goblin::add_collision
{
namespace
{
// RVAs (ERR 2.2.9.6, imagebase 0x140000000) — add_collision_linux_impl_brief.md. RVA now, AOB-harden later.
constexpr uintptr_t PHYSWORLD_RVA   = 0x3d76060;   // CS::PhysWorld FD4Singleton static slot (== heightfield)
constexpr uintptr_t CINFO_INIT_RVA  = 0x1911210;   // hknpBodyCinfo init (defaults)

using CinfoInitFn = void (*)(void *cinfo);

bool rd(const void *addr, void *out, size_t n)
{
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}
void *rdp(const void *addr) { void *v = nullptr; return rd(addr, &v, sizeof(v)) ? v : nullptr; }

// noinline so the __try wraps a lone opaque CALL (clang-cl elides __try around an inlined call).
__declspec(noinline) static bool call_cinfo_init(CinfoInitFn fn, void *cinfo)
{
    __try { fn(cinfo); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t er_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe")); }

// Resolve world + bodyMgr (== world) from the PhysWorld singleton. Fills r.err on failure.
bool resolve(Result &r)
{
    uintptr_t er = er_base();
    if (!er) { std::snprintf(r.err, sizeof(r.err), "eldenring.exe not found"); return false; }
    void *inst = rdp(reinterpret_cast<void *>(er + PHYSWORLD_RVA));
    if (reinterpret_cast<uintptr_t>(inst) < 0x10000) { std::snprintf(r.err, sizeof(r.err), "PhysWorld instance null (not in-world?)"); return false; }
    void *ctx = rdp(reinterpret_cast<uint8_t *>(inst) + 0x98);      // CSPhysWorld
    if (reinterpret_cast<uintptr_t>(ctx) < 0x10000) { std::snprintf(r.err, sizeof(r.err), "CSPhysWorld (*inst+0x98) null"); return false; }
    void *world = rdp(reinterpret_cast<uint8_t *>(ctx) + 0x08);     // hknpWorld == bodyMgr
    if (reinterpret_cast<uintptr_t>(world) < 0x10000) { std::snprintf(r.err, sizeof(r.err), "hknpWorld (*ctx+0x08) null"); return false; }
    void *bodies = rdp(reinterpret_cast<uint8_t *>(world) + 0x28);  // body array (0xb0 stride)
    uint32_t count = 0; rd(reinterpret_cast<uint8_t *>(world) + 0x30, &count, 4);
    r.world = reinterpret_cast<uint64_t>(world);
    r.bodyMgr = r.world;
    r.bodies = reinterpret_cast<uint64_t>(bodies);
    r.count = count;
    // Sanity: bodies ptr plausible + count sane (the brief's gate before trusting the layout).
    if (reinterpret_cast<uintptr_t>(bodies) < 0x10000 || count == 0 || count > 0x100000)
    {
        std::snprintf(r.err, sizeof(r.err), "bodyMgr layout implausible: bodies=%p count=%u", bodies, count);
        return false;
    }
    return true;
}

void hexdump(const char *tag, const uint8_t *buf, size_t n, uint64_t base_addr)
{
    char line[160];
    uintptr_t er = er_base();
    for (size_t i = 0; i < n; i += 16)
    {
        int p = std::snprintf(line, sizeof(line), "[ADDCOL] %s +0x%03zx:", tag, i);
        for (size_t j = 0; j < 16 && i + j < n; ++j)
            p += std::snprintf(line + p, sizeof(line) - p, " %02x", buf[i + j]);
        spdlog::info("{}", line);
    }
    // Also interpret each qword as a possible er-relative ptr (shape/vtable spotting).
    for (size_t i = 0; i + 8 <= n; i += 8)
    {
        uint64_t q; std::memcpy(&q, buf + i, 8);
        if (q > er && q < er + 0x10000000)
            spdlog::info("[ADDCOL] {} +0x{:03x} = er+0x{:x} (ptr?)", tag, i, (uintptr_t)q - er);
        else
        {
            float f0, f1; std::memcpy(&f0, buf + i, 4); std::memcpy(&f1, buf + i + 4, 4);
            if ((f0 > -1e6f && f0 < 1e6f && f0 != 0.f) || (f1 > -1e6f && f1 < 1e6f && f1 != 0.f))
                spdlog::info("[ADDCOL] {} +0x{:03x} = floats {:.2f} {:.2f}", tag, i, f0, f1);
        }
    }
}
} // namespace

Result resolve_world()
{
    Result r;
    r.ok = resolve(r);
    return r;
}

Result recon()
{
    Result r;
    if (!resolve(r)) return r;
    spdlog::info("[ADDCOL] resolve OK: world/bodyMgr={:#x} bodies={:#x} count={}", r.world, r.bodies, r.count);

    // 1) hknpBodyCinfo defaults — call init on a scratch buffer (aligned; Havok reads vectors via movaps).
    uintptr_t er = er_base();
    auto init = reinterpret_cast<CinfoInitFn>(er + CINFO_INIT_RVA);
    alignas(16) uint8_t cinfo[0x100];
    std::memset(cinfo, 0xCD, sizeof(cinfo));   // poison → init'd fields stand out vs 0xCD
    if (call_cinfo_init(init, cinfo))
    {
        spdlog::info("[ADDCOL] --- hknpBodyCinfo defaults (0xCD = untouched) ---");
        hexdump("cinfo", cinfo, 0xA0, 0);
    }
    else spdlog::warn("[ADDCOL] cinfo init FAULTED (bad RVA or ABI?)");

    // 2) A real, known-good body header (0xb0 stride) — to learn shape/pos/motion offsets by correlation.
    uint8_t body[0xB0];
    if (rd(reinterpret_cast<void *>(r.bodies), body, sizeof(body)))
    {
        spdlog::info("[ADDCOL] --- body[0] @ {:#x} (0xb0) ---", r.bodies);
        hexdump("body0", body, 0xB0, r.bodies);
    }
    else spdlog::warn("[ADDCOL] body[0] read failed");
    r.ok = true;   // recon ran (resolve + dumps); the field-mapping is read off the [ADDCOL] log
    return r;
}
} // namespace goblin::add_collision
