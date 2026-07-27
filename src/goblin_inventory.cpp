#include "goblin_inventory.hpp"

#include "goblin_debug_events.hpp"  // last_inventory_accessor — fallback inv
#include "modutils.hpp"
#include "re_signatures.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::inventory
{
namespace
{
// AddItemFunc(rcx=inv, rdx=&entry, r8=buffer, r9=count(0)). 4 args; extra regs ignored.
using AddItemFn = uint64_t(*)(void *inv, void *entry, void *buffer, uint64_t count);

AddItemFn g_add_item = nullptr;
void **g_inv_slot = nullptr;   // *g_inv_slot == the live MapItemMan (re-read each call)
std::atomic<bool> g_ready{false};

// The itembuffer replicated verbatim from the CT's ItemGib (10 qwords). Only qword[4] (the
// entry at +0x20) is patched per call: low dword = qty (i32), high dword = item id. The
// trailing 0xFFFFFFFF sentinels terminate the entry list — kept exactly as the CT sets them.
constexpr uint64_t kBufTemplate[10] = {
    0, 0, 0, 0,
    0xF00006AE00000001ull,  // [4] entry {qty=1, id=F00006AE} — patched per call
    0x0000000000000001ull,
    0xFFFFFFFFFFFFFFFFull,
    0xFFFFFFFF00000000ull,
    0xFFFFFFFFFFFFFFFFull,
    0xFFFFFFFF00000000ull,
};
}  // namespace

void initialize()
{
    if (g_ready.load()) return;
    // AddItemFunc — its own prologue AOB.
    g_add_item = reinterpret_cast<AddItemFn>(modutils::scan<void>({.aob = goblin::sig::ADD_ITEM_FUNC}));
    // MapItemMan slot from INVENTORY_ACCESSOR: the `mov rcx,[rip+disp32]` at match+0x16 →
    // slot = (match + 0x1D) + *(int32*)(match + 0x19).
    if (auto *m = reinterpret_cast<uint8_t *>(
            modutils::scan<void>({.aob = goblin::sig::INVENTORY_ACCESSOR})))
    {
        int32_t disp = *reinterpret_cast<int32_t *>(m + 0x19);
        g_inv_slot = reinterpret_cast<void **>(m + 0x1D + disp);
    }
    g_ready.store(true);
    spdlog::info("[INVGRANT] AddItemFunc={} inv_slot={} (accessor AOB {})",
                 (void *)g_add_item, (void *)g_inv_slot, g_inv_slot ? "OK" : "MISS");
}

void *accessor()
{
    // Live re-read of the static slot (the singleton can be created after init). Fall back to
    // the observer-captured pointer if the AOB didn't resolve.
    if (g_inv_slot)
    {
        // RPM (kernel call, not elidable, false on a bad ptr) — the slot may be unmapped
        // before the singleton exists.
        void *inv = nullptr;
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), g_inv_slot, &inv, sizeof(inv), &got) &&
            got == sizeof(inv) && reinterpret_cast<uintptr_t>(inv) >= 0x10000)
            return inv;
    }
    return goblin::debug_events::last_inventory_accessor();
}

// The raw game call, isolated so the caller's __try wraps a lone opaque CALL (clang-cl keeps
// that; a __try around inline loads gets elided — see clang-cl-seh-noinline).
__declspec(noinline) static uint64_t call_add_item(void *inv, void *entry, void *buffer)
{
    return g_add_item(inv, entry, buffer, 0);
}

static bool give_item_seh(void *inv, void *entry, void *buffer)
{
    __try
    {
        call_add_item(inv, entry, buffer);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

namespace
{
// GameDataMan static slot, resolved once from the GAME_DATA_MAN getter AOB and shared by the
// equip chain, the bloodstain read, and the debug dump. match keeps the raw AOB hit so the
// debug RPC can report er-relative addresses (2.6.2.0 triage: the "AOB drift" suspicion was a
// file-offset-vs-RVA confusion; dumping the real resolved chain settles it live).
uint8_t *g_gdm_match = nullptr;
void **game_data_man_slot()
{
    static void **slot = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        if (auto *m = reinterpret_cast<uint8_t *>(
                modutils::scan<void>({.aob = goblin::sig::GAME_DATA_MAN})))
        {
            g_gdm_match = m;
            int32_t disp = *reinterpret_cast<int32_t *>(m + 3);
            slot = reinterpret_cast<void **>(m + 7 + disp);
        }
        spdlog::info("[EQUIP] GameDataMan slot={}", (void *)slot);
    }
    return slot;
}
}  // namespace

void *equip_game_data()
{
    // GameDataMan slot (cached), then walk the chain with RPM (guarded — nulls before the world
    // loads). GameDataMan = *((match+7) + disp32@(match+3)).
    void **gdm_slot = game_data_man_slot();
    if (!gdm_slot) return nullptr;
    auto rd = [](void *p) -> void * {
        void *v = nullptr;
        SIZE_T got = 0;
        return (p && ReadProcessMemory(GetCurrentProcess(), p, &v, sizeof(v), &got) &&
                got == sizeof(v) && reinterpret_cast<uintptr_t>(v) >= 0x10000)
                   ? v
                   : nullptr;
    };
    void *gdm = rd(gdm_slot);
    void *pgd = rd(gdm ? reinterpret_cast<uint8_t *>(gdm) + 0x8 : nullptr);
    void *egd = pgd ? reinterpret_cast<uint8_t *>(pgd) + 0x2B0 : nullptr;
    static void *last_pgd = nullptr;
    if (pgd && pgd != last_pgd)
    {
        last_pgd = pgd;
        spdlog::info("[EQUIP] chain: GameDataMan={} PlayerGameData={} EquipGameData={}", gdm, pgd, egd);
    }
    return egd;
}

// Run-tracker counters (deaths + in-game time) off the same cached GameDataMan slot. Both fields
// live in the block the save serializer streams, next to the bloodstain record below: deaths at
// +0x94, in-game time at +0xA0 as u32 milliseconds. RPM-guarded like the equip walk, so a call
// before the world loads (slot resolved, pointer still null) just returns false.
//
// The two offsets are the ones soarqin/EROverlay's boss overlay reads (its `kDeathCount`/
// `kInGameTime`); +0x94 is additionally proven here by the death-marker path. Read-only.
bool read_run_stats(uint32_t &deaths, uint32_t &igt_ms)
{
    void **gdm_slot = game_data_man_slot();
    if (!gdm_slot) return false;
    HANDLE self = GetCurrentProcess();
    void *gdm = nullptr;
    SIZE_T got = 0;
    if (!ReadProcessMemory(self, gdm_slot, &gdm, sizeof(gdm), &got) || got != sizeof(gdm) ||
        reinterpret_cast<uintptr_t>(gdm) < 0x10000)
        return false;  // title screen / loading — no game data yet
    uint32_t d = 0, t = 0;
    SIZE_T n1 = 0, n2 = 0;
    if (!ReadProcessMemory(self, reinterpret_cast<uint8_t *>(gdm) + 0x94, &d, sizeof(d), &n1) ||
        n1 != sizeof(d) ||
        !ReadProcessMemory(self, reinterpret_cast<uint8_t *>(gdm) + 0xA0, &t, sizeof(t), &n2) ||
        n2 != sizeof(t))
        return false;
    deaths = d;
    igt_ms = t;
    return true;
}

// Native persistent bloodstain (dropped runes on death) — 2.6.2.0 Ghidra-verified layout
// (docs/re/windows_bloodstain_read_drift_re_findings.md; supersedes the Hexinton-CT note):
// GameDataMan+0x40 (u8) = EXISTS flag — the engine's own gate (setter er+0x259060, read by the
// icon placer er+0x5fbc70). Set on ANY death, INCLUDING a 0-rune one, restored from the save
// (deserialize er+0x256be0 streams the flag + the 0x40-byte record verbatim). blk =
// [GameDataMan+0x48]: X/Y/Z @ +0/+4/+8 (block-local frame, engine-REBASED on origin moves),
// runes @ +0x34 (death writer er+0x5fc0c0 stamps PlayerGameData+0x6C there; -1 = cleared,
// 0 = a real 0-rune stain), mapId @ +0x38. Save-backed → persists + auto-clears on pickup.
// ⚠ exists must NOT be derived from souls>0 — that hid every 0-rune bloodstain (the 2026-07-08
// "read is broken" symptom: ER drew its icon, the mod suppressed its marker).
bool read_bloodstain(bool &exists, float &x, float &y, float &z, uint32_t &mapid, int32_t &souls)
{
    void **gdm_slot = game_data_man_slot();
    if (!gdm_slot) return false;
    auto rd = [](void *p, void *out, SIZE_T n) -> bool {
        SIZE_T got = 0;
        return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
    };
    void *gdm = nullptr;
    if (!rd(gdm_slot, &gdm, sizeof(gdm)) || reinterpret_cast<uintptr_t>(gdm) < 0x10000) return false;
    uint8_t flag = 0;
    if (!rd(reinterpret_cast<uint8_t *>(gdm) + 0x40, &flag, 1)) return false;
    uint8_t *blk = nullptr;
    if (!rd(reinterpret_cast<uint8_t *>(gdm) + 0x48, &blk, sizeof(blk)) ||
        reinterpret_cast<uintptr_t>(blk) < 0x10000)
        return false;
    if (!(rd(blk + 0x0, &x, 4) && rd(blk + 0x4, &y, 4) && rd(blk + 0x8, &z, 4) &&
          rd(blk + 0x38, &mapid, 4) && rd(blk + 0x34, &souls, 4)))
        return false;
    exists = flag != 0 && souls >= 0;
    return true;
}

std::string bloodstain_debug()
{
    void **gdm_slot = game_data_man_slot();
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    char b[512];
    if (!gdm_slot)
    {
        std::snprintf(b, sizeof(b), "gdm_slot UNRESOLVED (AOB miss) er=0x%llx",
                      (unsigned long long)er);
        return b;
    }
    auto rd = [](const void *p, void *out, SIZE_T n) -> bool {
        SIZE_T got = 0;
        return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
    };
    void *gdm = nullptr;
    rd(gdm_slot, &gdm, sizeof(gdm));
    uint8_t flag = 0;
    int32_t aux50 = 0;
    uint8_t *blk = nullptr;
    uint8_t raw[0x40] = {};
    bool blk_ok = false;
    if (reinterpret_cast<uintptr_t>(gdm) >= 0x10000)
    {
        rd(reinterpret_cast<uint8_t *>(gdm) + 0x40, &flag, 1);
        rd(reinterpret_cast<uint8_t *>(gdm) + 0x50, &aux50, 4);
        rd(reinterpret_cast<uint8_t *>(gdm) + 0x48, &blk, sizeof(blk));
        if (reinterpret_cast<uintptr_t>(blk) >= 0x10000) blk_ok = rd(blk, raw, sizeof(raw));
    }
    char hex[3 * sizeof(raw) + 1] = {};
    for (size_t i = 0; i < sizeof(raw); ++i)
        std::snprintf(hex + i * 3, 4, "%02x ", raw[i]);
    auto rel = [er](const void *p) -> long long {
        return er ? (long long)((uintptr_t)p - er) : -1;
    };
    std::snprintf(b, sizeof(b),
                  "match=er+0x%llx slot=er+0x%llx gdm=%p flag40=%u aux50=%d blk=%p rec[0x40]=%s%s",
                  (unsigned long long)rel(g_gdm_match), (unsigned long long)rel(gdm_slot), gdm,
                  flag, aux50, (void *)blk, blk_ok ? hex : "<unreadable>",
                  blk_ok ? "" : " (blk read failed)");
    return b;
}

namespace
{
// One guarded RPM read of a POD from the game address space. false on a bad/unmapped ptr (the
// inventory chain is null before the world loads) — never faults the DLL.
template <typename T>
bool rpm(const void *p, T &out)
{
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, &out, sizeof(T), &got) && got == sizeof(T);
}

// Direct in-process writes under SEH. WriteProcessMemory-to-self silently fails on the inventory
// pages here (live-verified: qty stayed 6 after a WPM strip; a direct store zeroed it), so the
// strip/restore node edits use these instead. noinline so the __try wraps a real store.
__declspec(noinline) static bool write_dw(void *p, uint32_t v)
{
    __try { *reinterpret_cast<volatile uint32_t *>(p) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) static bool write_bytes(void *p, const void *src, size_t n)
{
    __try { std::memcpy(p, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Root layout canary for the inventory chain ──────────────────────────────────────────────────
// The GameDataMan+0x8 -> PlayerGameData+0x2B0 -> EquipGameData+0x158 -> EquipInventoryData chain and
// its segment fields (+0x1C count1, +0x80 last, +0x50/+0x40 seg bases, node stride 0x18) are hardcoded
// RE'd offsets with NO [SIG] coverage — a game patch that reshapes these structs would make strip/count
// read the WRONG memory SILENTLY. This asserts the STRUCTURAL invariants of a valid EquipInventoryData
// before the write path trusts them (trick 2, docs/re/fragile_primitives_audit.md). The per-node id
// match in strip_goods (trick 1) already prevents an actual bad write; the canary's job is to turn a
// silently-wrong layout into a LOUD log + a disabled strip instead of a mystery "stripped nothing".
//   verified -> latched (layout is process-stable, so this is ~free after the first OK).
//   conclusive violation (reads OK but invariants broken) -> log once + return false (strip SKIPS).
//   inconclusive (not in-world / empty / torn read) -> permissive true (don't latch, don't cry wolf).
bool verify_inventory_layout(void *egd)
{
    static std::atomic<bool> verified{false};
    if (verified.load(std::memory_order_relaxed)) return true;
    if (!egd) return true;  // absent — caller already gates on equip_game_data()
    auto *inv = reinterpret_cast<uint8_t *>(egd) + 0x158;

    uint32_t seg1 = 0;
    int32_t last = -1;
    uint64_t b1 = 0, b2 = 0;
    if (!rpm(inv + 0x1C, seg1) || !rpm(inv + 0x80, last) || !rpm(inv + 0x50, b1) || !rpm(inv + 0x40, b2))
        return true;                       // chain not resident yet — inconclusive
    if (last < 0) return true;             // empty inventory — nothing to validate
    const uint32_t span = static_cast<uint32_t>(last) + 1u;
    if (span > 0x4000) return true;        // torn/garbage read — inconclusive, don't latch a false alarm

    auto plausible = [](uint64_t p) { return p >= 0x10000 && (p & 0x7) == 0; };
    bool ok = seg1 <= span                 // count-in-segment-1 can never exceed the total
              && (seg1 == 0 || plausible(b1))
              && (span <= seg1 || plausible(b2));   // seg2 only used when span > seg1
    if (ok)                                // the LAST node must be readable at the derived stride
    {
        const uint32_t li = span - 1;
        const uint64_t base = (li < seg1) ? b1 : b2;
        const uint32_t idx = (li < seg1) ? li : li - seg1;
        uint64_t probe = 0;
        ok = rpm(reinterpret_cast<void *>(base + static_cast<uint64_t>(idx) * 0x18), probe);
    }
    if (ok)
    {
        verified.store(true, std::memory_order_relaxed);
        spdlog::info("[INVLAYOUT] EquipInventoryData canary OK (seg1={} span={})", seg1, span);
        return true;
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        spdlog::error("[INVLAYOUT] EquipInventoryData canary FAILED — chain layout shifted "
                      "(seg1={} span={} b1={:#x} b2={:#x}); inventory strip DISABLED to protect the save. "
                      "Game patch? Re-RE the chain: docs/re/windows_goods_count_re_findings.md",
                      seg1, span, b1, b2);
    return false;
}
}  // namespace

uint32_t goods_count(uint32_t item_id)
{
    // EquipInventoryData (carried) = EquipGameData + 0x158. Walk the two-segment slot list read-only.
    // Layout + node fields: docs/re/windows_goods_count_re_findings.md.
    void *egd = equip_game_data();
    if (!egd) return 0;  // not in-world yet — caller gates on equip_game_data() to tell absent apart.
    if (!verify_inventory_layout(egd)) return 0;  // root canary: bail on a shifted layout (logged once)
    auto *inv = reinterpret_cast<uint8_t *>(egd) + 0x158;

    uint32_t seg1 = 0;
    int32_t last = -1;
    uint64_t seg1_base = 0, seg2_base = 0;
    if (!rpm(inv + 0x1C, seg1) || !rpm(inv + 0x80, last) ||
        !rpm(inv + 0x50, seg1_base) || !rpm(inv + 0x40, seg2_base))
        return 0;
    if (last < 0) return 0;

    // Sanity clamp — a torn read mid-transition shouldn't spin the DLL on a garbage count.
    const uint32_t span = static_cast<uint32_t>(last) + 1u;
    if (span > 0x4000) return 0;

    uint64_t total = 0;
    for (uint32_t i = 0; i < span; ++i)
    {
        uint64_t base = (i < seg1) ? seg1_base : seg2_base;
        uint32_t idx = (i < seg1) ? i : i - seg1;
        auto *node = reinterpret_cast<const uint8_t *>(base) + static_cast<uint64_t>(idx) * 0x18;

        uint32_t active = 0, node_id = 0, qty = 0;
        if (!rpm(node, active) || active == 0) continue;   // empty slot
        if (!rpm(node + 4, node_id) || node_id != item_id) continue;
        if (rpm(node + 8, qty)) total += qty;
    }
    // Held stacks are u32; clamp defensively.
    return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
}

std::vector<StripEntry> strip_goods(const std::vector<uint32_t> &ids)
{
    std::vector<StripEntry> saved;
    if (ids.empty()) return saved;
    void *egd = equip_game_data();
    if (!egd) return saved;  // not in-world
    if (!verify_inventory_layout(egd)) return saved;  // root canary: never write into a shifted layout
    auto *inv = reinterpret_cast<uint8_t *>(egd) + 0x158;

    uint32_t seg1 = 0;
    int32_t last = -1;
    uint64_t seg1_base = 0, seg2_base = 0;
    if (!rpm(inv + 0x1C, seg1) || !rpm(inv + 0x80, last) ||
        !rpm(inv + 0x50, seg1_base) || !rpm(inv + 0x40, seg2_base))
        return saved;
    if (last < 0) return saved;
    const uint32_t span = static_cast<uint32_t>(last) + 1u;
    if (span > 0x4000) return saved;

    for (uint32_t i = 0; i < span; ++i)
    {
        uint64_t base = (i < seg1) ? seg1_base : seg2_base;
        uint32_t idx = (i < seg1) ? i : i - seg1;
        auto *node = reinterpret_cast<uint8_t *>(base) + static_cast<uint64_t>(idx) * 0x18;

        uint32_t active = 0, node_id = 0;
        if (!rpm(node, active) || active == 0) continue;
        if (!rpm(node + 4, node_id)) continue;
        bool match = false;
        for (uint32_t id : ids)
            if (node_id == id) { match = true; break; }
        if (!match) continue;

        StripEntry e{};
        e.node = reinterpret_cast<uintptr_t>(node);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), node, e.bytes, sizeof(e.bytes), &got) ||
            got != sizeof(e.bytes))
            continue;
        // Mark the slot empty for the serialize: zero the handle (@0, the active flag the game's
        // own iteration idiom checks) and the qty (@8). Restored byte-exact by restore_goods().
        write_dw(node, 0);
        write_dw(node + 8, 0);
        saved.push_back(e);
    }
    if (!saved.empty())
        spdlog::info("[INVSTRIP] stripped {} node(s) matching {} id(s)", saved.size(), ids.size());
    return saved;
}

void restore_goods(const std::vector<StripEntry> &saved)
{
    for (const auto &e : saved)
        write_bytes(reinterpret_cast<void *>(e.node), e.bytes, sizeof(e.bytes));
    if (!saved.empty()) spdlog::info("[INVSTRIP] restored {} node(s)", saved.size());
}

bool give_item(uint32_t item_id, int32_t qty)
{
    if (!g_ready.load()) initialize();
    if (!g_add_item) { spdlog::warn("[INVGRANT] AddItemFunc unresolved — give_item skipped"); return false; }
    void *inv = accessor();
    if (!inv) { spdlog::warn("[INVGRANT] no inventory accessor (no grant seen + AOB miss) — skipped"); return false; }

    uint64_t buf[10];
    std::memcpy(buf, kBufTemplate, sizeof(buf));
    // entry @ +0x20 = buf[4]: low dword = qty (as raw i32 bits), high dword = item id.
    buf[4] = (static_cast<uint64_t>(item_id) << 32) | static_cast<uint32_t>(qty);

    void *entry = reinterpret_cast<uint8_t *>(buf) + 0x20;  // rdx
    bool ok = give_item_seh(inv, entry, buf);               // r8 = buf base
    spdlog::info("[INVGRANT] give_item id={:#010x} qty={} inv={} -> {}", item_id, qty, inv,
                 ok ? "called" : "FAULTED");
    return ok;
}
}  // namespace goblin::inventory
