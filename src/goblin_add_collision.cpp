#include "goblin_add_collision.hpp"

#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <spdlog/spdlog.h>

#include "re_signatures.hpp"   // physworld_slot (AOB-first) — the PHYSWORLD RVA hardening

namespace goblin::add_collision
{
namespace
{
// RVAs (ERR 2.2.9.6, imagebase 0x140000000) — add_collision_linux_impl_brief.md. RVA now, AOB-harden later.
constexpr uintptr_t PHYSWORLD_RVA     = 0x3d76060;  // CS::PhysWorld FD4Singleton static slot (== heightfield)
constexpr uintptr_t CINFO_INIT_RVA    = 0x1911210;  // hknpBodyCinfo init (defaults)
constexpr uintptr_t ALLOCATE_BODY_RVA = 0x18aabf0;  // hknpBodyManager::allocateBody(bodyMgr, &outId, &cinfo)
constexpr uintptr_t ADD_BODY_RVA      = 0x18a9ff0;  // addBody(bodyMgr, ids, count, addMode, actMode)

using CinfoInitFn = void (*)(void *cinfo);
using AllocBodyFn = uint32_t *(*)(void *bodyMgr, uint32_t *outId, void *cinfo);
using AddBodyFn   = void (*)(void *bodyMgr, uint32_t *ids, uint32_t count, int addMode, int actMode);

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

__declspec(noinline) static bool call_alloc_body(AllocBodyFn fn, void *mgr, uint32_t *id, void *cinfo)
{
    __try { fn(mgr, id, cinfo); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) static bool call_add_body(AddBodyFn fn, void *mgr, uint32_t *ids, uint32_t n,
                                               int addMode, int actMode)
{
    __try { fn(mgr, ids, n, addMode, actMode); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t er_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe")); }

// Resolve world + bodyMgr (== world) from the PhysWorld singleton. Fills r.err on failure.
bool resolve(Result &r)
{
    uintptr_t er = er_base();
    if (!er) { std::snprintf(r.err, sizeof(r.err), "eldenring.exe not found"); return false; }
    static void **slot = goblin::sig::physworld_slot(er, PHYSWORLD_RVA);   // scan once per process
    void *inst = rdp(slot);
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

namespace
{
// Borrowable live shape: walk the body array (0xb0 stride) and take the first body whose shape ptr
// (recon: body+0x60) has a vtable in the hknp-shape neighbourhood (er+0x2ee0000..0x2ef0000, excluding
// the hknpWorld vtable 0x2eedc78 — the hf_shape_probe heuristic). Returns the shape + its vtable RVA.
void *find_live_shape(const Result &r, uintptr_t er, uintptr_t &vt_rva_out)
{
    const uint32_t scan = r.count < 256 ? r.count : 256;
    for (uint32_t i = 0; i < scan; ++i)
    {
        void *shape = rdp(reinterpret_cast<uint8_t *>(r.bodies) + (uint64_t)i * 0xB0 + 0x60);
        if (reinterpret_cast<uintptr_t>(shape) < 0x10000) continue;
        uintptr_t vt = reinterpret_cast<uintptr_t>(rdp(shape));
        if (vt <= er) continue;
        uintptr_t rva = vt - er;
        if (rva >= 0x2ee0000 && rva < 0x2ef0000 && rva != 0x2eedc78)
        {
            vt_rva_out = rva;
            return shape;
        }
    }
    return nullptr;
}
} // namespace

Result add_box(const float half[3], const float pos[3], bool force)
{
    Result r;
    if (!resolve(r)) return r;
    std::memcpy(r.half, half, sizeof(r.half));
    std::memcpy(r.pos, pos, sizeof(r.pos));
    uintptr_t er = er_base();

    // First-probe shortcut (findings §6): borrow a live body's shape instead of building the box —
    // no refcount bump (the source body outlives a probe; the real box builder replaces this).
    uintptr_t shape_vt_rva = 0;
    void *shape = find_live_shape(r, er, shape_vt_rva);
    if (!shape)
    {
        std::snprintf(r.err, sizeof(r.err), "no borrowable hknp shape in body array (scanned %u)", r.count);
        return r;
    }
    r.shape = reinterpret_cast<uint64_t>(shape);

    // cinfo: engine defaults, then the minimal STATIC fill — shape@+0x00, position@+0x30; orientation
    // (+0x40) stays identity, motionType (+0x28) stays 0 = STATIC, id/motion sentinels auto-allocate.
    alignas(16) uint8_t cinfo[0x100];
    std::memset(cinfo, 0, sizeof(cinfo));
    auto init = reinterpret_cast<CinfoInitFn>(
        goblin::sig::resolve_func_aob(goblin::sig::CINFO_INIT_FN, er, CINFO_INIT_RVA, "CINFO_INIT"));
    if (!call_cinfo_init(init, cinfo))
    {
        std::snprintf(r.err, sizeof(r.err), "cinfo init faulted");
        return r;
    }
    std::memcpy(cinfo + 0x00, &shape, 8);
    std::memcpy(cinfo + 0x30, pos, 12);

    spdlog::info("[ADDCOL] add_box: shape={} (vt er+0x{:x}, BORROWED — half=({:.0f},{:.0f},{:.0f}) "
                 "informational until the box builder) pos=({:.1f},{:.1f},{:.1f}) motion=STATIC",
                 shape, shape_vt_rva, half[0], half[1], half[2], pos[0], pos[1], pos[2]);
    hexdump("cinfo.filled", cinfo, 0xA0, 0);

    if (!force)
    {
        r.ok = true;
        spdlog::info("[ADDCOL] add_box: dump-only (no alloc/add) — append 'go' to fire");
        return r;
    }

    uint32_t id = 0xffffffffu;
    auto alloc = reinterpret_cast<AllocBodyFn>(
        goblin::sig::resolve_func_aob(goblin::sig::ALLOCATE_BODY_FN, er, ALLOCATE_BODY_RVA, "ALLOCATE_BODY"));
    if (!call_alloc_body(alloc, reinterpret_cast<void *>(r.bodyMgr), &id, cinfo))
    {
        std::snprintf(r.err, sizeof(r.err), "allocateBody FAULTED (cinfo layout / bodyMgr wrong?)");
        return r;
    }
    if ((id & 0xffffffu) == 0xffffffu)
    {
        std::snprintf(r.err, sizeof(r.err), "allocateBody returned invalid id 0x%x", id);
        return r;
    }
    r.bodyId = id;
    spdlog::info("[ADDCOL] allocateBody OK: id=0x{:x} — calling addBody(addMode=0, actMode=0)", id);

    auto add = reinterpret_cast<AddBodyFn>(
        goblin::sig::resolve_func_aob(goblin::sig::ADD_BODY_FN, er, ADD_BODY_RVA, "ADD_BODY"));
    if (!call_add_body(add, reinterpret_cast<void *>(r.bodyMgr), &id, 1, 0, 0))
    {
        std::snprintf(r.err, sizeof(r.err), "addBody FAULTED (id=0x%x allocated but not added)", id);
        return r;
    }

    // Record the new body slot (0xb0 @ bodies + (id&0xffffff)*0xb0) for the findings.
    uint64_t slot = r.bodies + (uint64_t)(id & 0xffffffu) * 0xB0;
    uint8_t body[0xB0];
    if (rd(reinterpret_cast<void *>(slot), body, sizeof(body)))
    {
        spdlog::info("[ADDCOL] --- new body @ {:#x} (id 0x{:x}) ---", slot, id);
        hexdump("newbody", body, 0xB0, slot);
    }
    r.ok = true;
    spdlog::info("[ADDCOL] add_box OK: bodyId=0x{:x} slot={:#x} — verify: hf_probe_present over "
                 "({:.1f},{:.1f},{:.1f}) should hit the body top", id, slot, pos[0], pos[1], pos[2]);
    return r;
}
} // namespace goblin::add_collision
