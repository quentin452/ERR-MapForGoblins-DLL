#include "goblin_inject.hpp"
#include "goblin_inject_shared.hpp"
#include "goblin_config.hpp"
#include "goblin_map_data.hpp"
#include "from/params.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <windows.h>

//
// Live loot flag/identity resolution — split out of goblin_inject.cpp 2026-07-01
// (docs/plans/goblin_inject_refactor_plan.md PR 0). Pure relocation, no logic
// changes. Owns the ItemLotParam raw reader (LotReader) and everything that
// consumes it: live pickup-flag resolve, live item-identity resolve, the
// EMEVD sub-lot probe, item-count, and the one-shot [LOOTDIAG] dump. Depends
// on goblin_inject.cpp's EventFlagMan resolve (orp_flag_set) via
// goblin_inject_shared.hpp — that resolve is genuinely shared infra used by
// several OTHER sections still in goblin_inject.cpp, so it stays there rather
// than being duplicated or dragged over here.
//

using Category = goblin::generated::Category;

// Number of marker categories (enum has no COUNT sentinel; keep in sync).
// Local duplicate of goblin_inject.cpp's NUM_CATEGORIES (same convention already
// used there for per-file AOB/RPM helper copies) — only diag_loot_flags needs it.
static constexpr int NUM_CATEGORIES = static_cast<int>(Category::WorldLegacyDungeon) + 1;

// Local duplicate of goblin_inject.cpp's icon_rpm_i32/icon_rpm_ptr (same small
// RPM-wrapper helpers, same per-file-copy convention as e.g. goblin_markers.cpp /
// goblin_kindling.cpp keeping their own AOB copies) — avoids depending on the
// icon-harvest block's anonymous namespace for a 5-line helper.
namespace
{
inline bool icon_rpm_i32(uintptr_t a, int &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 4, &n) && n == 4;
}
inline bool icon_rpm_ptr(uintptr_t a, uintptr_t &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 8, &n) && n == 8;
}
} // namespace

namespace
{
// One ItemLotParam row, read by raw offset (ITEMLOT_PARAM_ST = 152 bytes,
// shared layout for _map and _enemy).
struct RawItemLotRow { uint8_t b[0x98]; };

// Reads ItemLotParam_map / _enemy from live memory once, then resolves rows by
// id. Shared by inject_map_entries (live icon/category) and
// refresh_loot_from_itemlot (live hide-flags / labels).
struct LotReader
{
    std::optional<from::params::ParamTableSequence<RawItemLotRow>> map_lots, enemy_lots;
    void init()
    {
        // ParamTableSequence has a const member (not copy-assignable) → emplace.
        try { map_lots.emplace(from::params::get_param<RawItemLotRow>(L"ItemLotParam_map")); } catch (...) {}
        try { enemy_lots.emplace(from::params::get_param<RawItemLotRow>(L"ItemLotParam_enemy")); } catch (...) {}
    }
    bool ok() const { return map_lots.has_value() || enemy_lots.has_value(); }
    RawItemLotRow *row(uint32_t lot_id, uint8_t lot_type)
    {
        auto &pref  = (lot_type == 2) ? enemy_lots : map_lots;
        auto &other = (lot_type == 2) ? map_lots : enemy_lots;
        // try_get = non-throwing, non-logging: a missing lot (ERR/randomizer removed it)
        // is an expected miss, not an error — just fall back to the other table / baked.
        if (pref)  { if (RawItemLotRow *r = pref->try_get(lot_id))  return r; }
        if (other) { if (RawItemLotRow *r = other->try_get(lot_id)) return r; }
        return nullptr;
    }
};

// Encode a live item (id + ItemLotParam category 1-5) into the offset-encoded
// key used by marker textIds (and item_marker_category's key-range classifier).
inline int32_t encode_live_item(int32_t item_id, int32_t cat)
{
    switch (cat)
    {
        case 1: return item_id + 500000000;     // goods (GoodsName)
        // weapon AND ammo both live in WeaponName.fmg and use the +100M offset. The baker's
        // CATEGORY_OFFSETS and the default per-marker name path (copy_fmg_layered offset_base=100M,
        // real_id = key-100M → WeaponName) both key cat-2 at
        // +100M. The old ">=50M ? raw" matched only the liveLootLabels-all ammo_as_is copy (raw
        // ammo) — an extra PlaceName entry no marker/icon points at; using it here mis-encoded ammo
        // (the 81 uniform +100M "drifts" in the [LOOTID] probe). See loot_ammo_encoding_finding.md.
        case 2: return item_id + 100000000;     // weapon / ammo (WeaponName)
        case 3: return item_id + 200000000;     // protector (armour)
        case 4: return item_id + 300000000;     // accessory (talismans)
        case 5: return item_id + 400000000;     // gem (ashes of war)
        default: return 0;
    }
}

// Spoiler-free (config::anonymousLoot) constants. The generic label reuses the
// localized BloodMsg word "something" (id 32004) at the +950M encoding (copied
// into PlaceName by setup_messages). The icon is our gray "?" frame added to
// sprite 171 of the worldmap gfx (next free frame after the tinted variants).
constexpr int32_t ANON_LABEL_TEXTID = 950000000 + 32004;  // "something"
} // namespace

// ── Live persistence classifier (replaces the >=0x40000000 numeric repeatable-cut) ──
// A loot flag is a tracked one-time collectible IFF its GROUP node is allocated in the
// persistent EventFlagMan's std::map<uint group, Block> (mgr+0x38). This ports the game's
// own FUN_1405f9400 (IsEventFlag's bit-lookup) — group = flagId / divisor(mgr+0x1c), then a
// std::map::find on the group. RUNTIME-VALIDATED 2026-06-25 via live RPM (divisor=1000; node
// layout left@0/right@+0x10/_Isnil@+0x19(byte)/key@+0x20(uint); sweep: DLC 506/506 persistent,
// base 4979/4990 — proves groups are pre-allocated at save load, not lazy). The old numeric cut
// wrongly dropped ALL DLC one-time loot (its flags are >=0x40000000 too); this is the ground
// truth. See docs/re/resolve_loot_flag_dlc_fix_prompt.md + memory resolve-loot-flag-dlc-bug.
//
// Reuses orp_flag_set's manager resolve (g_orp_event_man_slot = the PRIMARY EVENT_FLAG_MAN_SLOT,
// NOT _ALT which is a different singleton). All reads RPM-guarded (clang-cl elides __try on raw
// loads). Memoized by group — resolve_loot_flag is per-marker on hot census/graying refreshes,
// called from both the render thread and the disk-build worker.
static std::mutex g_flag_persist_mtx;
static std::unordered_map<uint32_t, uint8_t> g_flag_persist_cache;  // group -> 1 persistent / 0 not
static bool g_flag_div_ok = false;
static uint32_t g_flag_div = 0;

// One RPM read of a std::_Tree node's fields we need (left@0x00, right@0x10, _Isnil@0x19, key@0x20).
static bool fg_read_node(uintptr_t n, uintptr_t &left, uintptr_t &right, uint8_t &isnil, uint32_t &key)
{
    if (!n) return false;
    uint8_t buf[0x24];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)n, buf, sizeof(buf), &got) || got != sizeof(buf))
        return false;
    left  = *reinterpret_cast<uintptr_t *>(buf + 0x00);
    right = *reinterpret_cast<uintptr_t *>(buf + 0x10);
    isnil = buf[0x19];
    key   = *reinterpret_cast<uint32_t *>(buf + 0x20);
    return true;
}

// Returns true if the query could be computed (manager + divisor resolved + walk completed),
// writing the verdict to `persistent`. Returns false when the manager isn't resolvable yet, so
// the caller can fall back to the legacy numeric heuristic without regressing.
static bool flag_query_persistent(uint32_t flagId, bool &persistent)
{
    if (!orp_manager_resolve_tried()) orp_flag_set(6001);  // warm the shared manager resolve once
    void **g_orp_event_man_slot = orp_event_man_slot_ptr();
    if (!g_orp_event_man_slot) return false;
    uintptr_t mgr = 0;
    if (!icon_rpm_ptr((uintptr_t)g_orp_event_man_slot, mgr) || !mgr) return false;
    if (!g_flag_div_ok)
    {
        int d = 0;
        if (!icon_rpm_i32(mgr + 0x1c, d) || d <= 0) return false;
        g_flag_div = (uint32_t)d;
        g_flag_div_ok = true;
    }
    const uint32_t group = flagId / g_flag_div;
    {
        std::lock_guard<std::mutex> lk(g_flag_persist_mtx);
        auto it = g_flag_persist_cache.find(group);
        if (it != g_flag_persist_cache.end()) { persistent = it->second != 0; return true; }
    }
    // std::map::find(group): descend to lower_bound, then exact-key test.
    uintptr_t head = 0;
    if (!icon_rpm_ptr(mgr + 0x38, head) || !head) return false;
    uintptr_t node = 0;
    if (!icon_rpm_ptr(head + 0x08, node) || !node) return false;  // root = head->_Parent
    uintptr_t result = head;
    bool ok = false;
    for (int guard = 0; guard < 128; ++guard)
    {
        uintptr_t left = 0, right = 0; uint8_t isnil = 1; uint32_t key = 0;
        if (!fg_read_node(node, left, right, isnil, key)) { return false; }
        if (isnil) { ok = true; break; }
        if (key < group) node = right;
        else { result = node; node = left; }
        if (!node) { ok = true; break; }
    }
    if (!ok) return false;
    bool found = false;
    if (result != head)
    {
        uintptr_t l = 0, r = 0; uint8_t ni = 1; uint32_t rk = 0;
        if (!fg_read_node(result, l, r, ni, rk)) return false;
        found = (rk == group);   // lower_bound gives key>=group; equal ⇒ exact match
    }
    persistent = found;
    {
        std::lock_guard<std::mutex> lk(g_flag_persist_mtx);
        g_flag_persist_cache.emplace(group, (uint8_t)(found ? 1 : 0));
    }
    return true;
}

// Should this loot flag be treated as repeatable/temp (NOT a tracked one-time collectible)?
// -1 = always re-droppable; flag 0 is NOT repeatable (Rune Arc is flag 0, and callers handle 0
// via their own "no flag" path before this). Otherwise the live group query is ground truth, with
// the old numeric cut kept ONLY as a pre-resolve fallback so behaviour never regresses.
static bool flag_is_repeatable(uint32_t flag)
{
    if (flag == 0xFFFFFFFFu) return true;
    if (flag == 0) return false;
    bool persistent = false;
    if (flag_query_persistent(flag, persistent))
        return !persistent;
    return flag >= 0x40000000u;  // manager not resolved yet → legacy heuristic
}

// ── Live-loot: hide loot markers on the LIVE item-lot pickup flag ──────
// Resolve a lot-backed loot marker's LIVE pickup flag from ItemLotParam (same
// lookup as refresh_loot_from_itemlot below), so the overlay map can detect
// collected loot WITHOUT the native injection running. ERR/Randomizer reassign
// loot flags, so the baked textDisableFlagId1 is often stale — the live
// getItemFlagId is authoritative. Returns baked_flag when the row isn't lot-backed
// or the lot can't be resolved (graceful fallback). Always on (the old live_loot_flags
// flag, which gated the NATIVE map's param rewrite, was removed in Phase 2b); this is a
// read-only resolve for the overlay's collected-detection, which must work regardless
// (else ERR-remapped loot like Golden Runes never registers as taken).
// The LotReader is cached after first use (params are loaded by map-open time).
uint32_t goblin::resolve_loot_flag(uint32_t lotId, uint8_t lotType, uint32_t baked_flag)
{
    if (lotType == 0 || lotId == 0)
        return baked_flag;
    static LotReader s_lots;
    static std::once_flag s_once;
    static bool s_ok = false;
    // Thread-safe one-time init: the disk-loot build now resolves on a WORKER
    // thread (map_entry_layer) concurrently with render-thread callers
    // (find_nearby_overworld / messages), so the old `if(!s_init)` race is real.
    std::call_once(s_once, [] { s_lots.init(); s_ok = s_lots.ok(); });
    if (!s_ok)
        return baked_flag;
    RawItemLotRow *row = s_lots.row(lotId, lotType);
    if (!row)
        return baked_flag;
    // getItemFlagId @ +0x80 (lot-wide). Live-CONFIRMED via the embedded find-what-accesses (the game's
    // getter `mov eax,[row+0x80]; cmp eax,-1`), but NOT runtime-resolved: that getter is byte-identical
    // to its 0x84-field sibling, so no disp-wildcarded AOB can disambiguate it (the offset is the only
    // difference — circular). Stays pinned, cross-validated live + guarded by check_param_offsets.py.
    uint32_t flag = *reinterpret_cast<uint32_t *>(row->b + 0x80);  // lot-wide getItemFlagId
    if (flag == 0)
    {
        // Single-item lots only (else a slot-1 flag would mark the whole row taken
        // while other loot remains) — mirrors refresh_loot_from_itemlot.
        int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
        if (item2 == 0)
            flag = *reinterpret_cast<uint32_t *>(row->b + 0x60);  // getItemFlagId01
    }
    uint32_t resolved = flag ? flag : baked_flag;
    // Non-persistent / repeatable flags have NO save-backed "obtained" bit, so they read false
    // forever. Treat them as NOT a tracked collectible: return 0 so the caller skips the marker
    // for graying + census (the dominant cause of the 100%-save over-report). Ground truth is the
    // LIVE group-allocation query (flag_is_repeatable) — the old `>=0x40000000` numeric cut
    // wrongly dropped all DLC one-time loot and survives only as a pre-resolve fallback inside it.
    // See docs/re/windows_collected_loot_flag_re_findings.md + resolve_loot_flag_dlc_fix_prompt.md.
    if (flag_is_repeatable(resolved))
        return 0;
    return resolved;
}

// Resolve a lot-backed marker's IDENTITY (name/icon key) from the LIVE ItemLotParam row, so the
// marker shows the item ERR/randomizer actually placed instead of the baked vanilla one. The bake
// stores the offset-encoded textId1 at extraction time; ERR can swap the lot's item → that bake
// drifts. We read the live row's slot-1 item (lotItemId01 @ +0x00, lotItemCategory01 @ +0x20 —
// same row resolve_loot_flag already reads: flag01@0x60, lot flag@0x80) and re-encode it via the
// shared encode_live_item(). Returns the baked key on any miss (no lot, no row, empty/invalid
// slot-1, or a multi-item lot where slot-1 doesn't represent the marker). The encoded key feeds
// both the marker label (FMG via liveLootLabels' PlaceName preload) and item_marker_category().
int32_t goblin::resolve_loot_item_textid(uint32_t lotId, uint8_t lotType, int32_t baked_textid)
{
    if (lotType == 0 || lotId == 0)
        return baked_textid;
    static LotReader s_lots;
    static std::once_flag s_once;
    static bool s_ok = false;
    // Thread-safe one-time init: the disk-loot build now resolves on a WORKER
    // thread (map_entry_layer) concurrently with render-thread callers
    // (find_nearby_overworld / messages), so the old `if(!s_init)` race is real.
    std::call_once(s_once, [] { s_lots.init(); s_ok = s_lots.ok(); });
    if (!s_ok)
        return baked_textid;
    RawItemLotRow *row = s_lots.row(lotId, lotType);
    if (!row)
        return baked_textid;
    const int32_t item1 = *reinterpret_cast<int32_t *>(row->b + 0x00);  // lotItemId01
    const int32_t cat1  = *reinterpret_cast<int32_t *>(row->b + 0x20);  // lotItemCategory01
    if (item1 <= 0 || cat1 < 1 || cat1 > 5)
        return baked_textid;
    const int32_t live = encode_live_item(item1, cat1);
    return live ? live : baked_textid;
}

// EMEVD sequence-sibling walk primitive (loot_emevd_drops mechanism C). Probes a single
// ItemLotParam table (no fallback) so the caller can detect the gap that ends a sub-lot
// chain. See build_disk_emevd_markers + docs/re/windows_enemy_loot_nobake_analysis.md §5b.
bool goblin::lot_row_in_table(uint32_t lot, uint8_t lotType, uint32_t *flagOut, int32_t *keyOut)
{
    if (flagOut) *flagOut = 0;
    if (keyOut) *keyOut = 0;
    if (lotType == 0 || lot == 0) return false;
    static LotReader s_lots;
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, [] { s_lots.init(); s_ok = s_lots.ok(); });
    if (!s_ok) return false;
    auto &tbl = (lotType == 2) ? s_lots.enemy_lots : s_lots.map_lots;  // SPECIFIC table only
    if (!tbl) return false;
    RawItemLotRow *row = tbl->try_get(lot);
    if (!row) return false;  // gap — the contiguous sub-lot chain ends here
    // Notability flag, same semantics as resolve_loot_flag (lot-wide @+0x80, single-item
    // fallback to getItemFlagId01 @+0x60; repeatable/temp → not notable, via the same live
    // group query so DLC one-time loot stays notable).
    uint32_t flag = *reinterpret_cast<uint32_t *>(row->b + 0x80);
    if (flag == 0)
    {
        int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
        if (item2 == 0) flag = *reinterpret_cast<uint32_t *>(row->b + 0x60);
    }
    if (flag_is_repeatable(flag)) flag = 0;
    if (flagOut) *flagOut = flag;
    // Slot-1 item → encoded key (same as resolve_loot_item_textid).
    int32_t item1 = *reinterpret_cast<int32_t *>(row->b + 0x00);
    int32_t cat1  = *reinterpret_cast<int32_t *>(row->b + 0x20);
    if (item1 > 0 && cat1 >= 1 && cat1 <= 5)
    {
        int32_t k = encode_live_item(item1, cat1);
        if (keyOut) *keyOut = k;
    }
    return true;
}

// Deterministic item quantity a single ItemLotParam lot grants. An ItemLotParam row has 8 item
// slots (lotItemId01..08 @ +0x00, lotItemCategory01..08 @ +0x20, lotItemBasePoint01..08 (u16,
// weight) @ +0x40, lotItemNum01..08 (u8) @ +0x8A) but the slots are a SINGLE WEIGHTED ROLL, not an
// additive bundle: the game picks one slot by basePoint weight and grants its (item × num). So
// summing the slots is wrong — a lot with slots (Formic num1, Formic num2) grants 1 OR 2, never 3.
// Several GUARANTEED items are encoded as sibling lot rows (base+1, base+2 — see emit_lot_siblings),
// each a separate one-slot lot, NOT as multiple slots of one row. Therefore a fixed quantity exists
// only when exactly one slot is live (item>0, valid category, basePoint>0); then it's that slot's
// num (e.g. a single-slot "5× arrows" lot → 5). Multiple live slots = RNG alternatives, so there is
// no fixed count → 1 (the guaranteed floor). Returns 1 on any miss. Live param chain, any mod, no
// bake — same reader as resolve_loot_item_textid. See docs/plans/loot_item_count_plan.md.
int goblin::lot_item_count(uint32_t lotId, uint8_t lotType)
{
    if (lotType == 0 || lotId == 0)
        return 1;
    static LotReader s_lots;
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, [] { s_lots.init(); s_ok = s_lots.ok(); });
    if (!s_ok)
        return 1;
    RawItemLotRow *row = s_lots.row(lotId, lotType);
    if (!row)
        return 1;
    int live = 0;        // number of weighted (basePoint>0) item slots
    int singleNum = 1;   // num of the sole live slot, used only when live == 1
    for (int i = 0; i < 8; ++i)
    {
        const int32_t item = *reinterpret_cast<int32_t *>(row->b + 0x00 + i * 4);   // lotItemId0(i+1)
        const int32_t cat  = *reinterpret_cast<int32_t *>(row->b + 0x20 + i * 4);   // lotItemCategory0(i+1)
        const uint16_t base = *reinterpret_cast<uint16_t *>(row->b + 0x40 + i * 2); // lotItemBasePoint0(i+1)
        if (item <= 0 || cat < 1 || cat > 5 || base == 0)
            continue;
        const uint8_t num = *(row->b + 0x8A + i);                                   // lotItemNum0(i+1)
        singleNum = (num > 0) ? num : 1;
        ++live;
    }
    return (live == 1) ? singleNum : 1;
}

// One-shot field dump to RE the real per-item "obtained" flag (see the findings doc).
// For up to ~6 markers per category, log every candidate flag and its live SET/unset
// state. Run on a 100% save: the candidate that reads SET for a known-collected item is
// the correct field; if NONE is set for whole categories, those lots carry no readable
// persistent obtained flag (repeatable, or the real flag isn't in the lot/map data).
void goblin::diag_loot_flags(uint32_t lotId, uint8_t lotType, uint32_t baked, int category,
                             uint32_t nameId)
{
    if (!goblin::config::diagLootFlags) return;
    static int per_cat[NUM_CATEGORIES] = {0};
    if (category < 0 || category >= NUM_CATEGORIES || per_cat[category] >= 6) return;
    auto setstr = [](uint32_t f) -> const char * {
        return f ? (orp_flag_set(f) ? "(SET)" : "(unset)") : "";
    };
    if (lotType == 0 || lotId == 0)
    {
        spdlog::info("[LOOTDIAG] cat {:2} name {} NO-LOT baked={}{}", category, nameId, baked,
                     setstr(baked));
        per_cat[category]++;
        return;
    }
    static LotReader s_diag;
    static std::once_flag s_diag_once;
    static bool s_ok = false;
    std::call_once(s_diag_once, [] { s_diag.init(); s_ok = s_diag.ok(); });
    RawItemLotRow *row = s_ok ? s_diag.row(lotId, lotType) : nullptr;
    if (!row)
    {
        spdlog::info("[LOOTDIAG] cat {:2} name {} lot {}/{} NO-ROW baked={}{}", category, nameId,
                     lotId, lotType, baked, setstr(baked));
        per_cat[category]++;
        return;
    }
    int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
    uint32_t lotwide = *reinterpret_cast<uint32_t *>(row->b + 0x80);
    std::string slots;
    for (int i = 0; i < 8; ++i)
    {
        uint32_t f = *reinterpret_cast<uint32_t *>(row->b + 0x60 + i * 4);
        slots += std::to_string(f);
        slots += setstr(f);
        slots += ' ';
    }
    spdlog::info("[LOOTDIAG] cat {:2} name {} lot {}/{} item2={} | @0x80={}{} | baked={}{} | "
                 "slots60: {}",
                 category, nameId, lotId, lotType, item2, lotwide, setstr(lotwide), baked,
                 setstr(baked), slots);
    per_cat[category]++;
}

