#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "goblin_inject.hpp"  // goblin::is_section_hidden_ptr
#include "goblin_map_data.hpp"
#include "goblin_geof_models.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "goblin_bench.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <string>
#include <tuple>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using Category = goblin::generated::Category;

// Model IDs are: 10,000,000 + AEG_group*1000 + model_number
// e.g. AEG099_821 = 10,099,821 = 0x009A1C6D
static constexpr uint16_t GEOM_IDX_MIN = 0x1194;

// ─── data-driven model tracking ─────────────────────────────────────
// Built from MAP_ENTRIES at init time — no hardcoded model lists.

static std::set<std::string> g_tracked_prefixes;   // "AEG099_821", "AEG099_651", etc.
static std::set<uint32_t>    g_tracked_model_ids;   // 10099821, 10099651, etc.

static std::string prefix_from_object_name(const char *name)
{
    // "AEG099_821_9000" → "AEG099_821"
    const char *last_us = strrchr(name, '_');
    if (!last_us || last_us == name) return {};
    return std::string(name, last_us - name);
}

static std::string prefix_from_model_id(uint32_t model_id)
{
    if (model_id < 10000000) return {};
    uint32_t raw = model_id - 10000000;
    uint32_t group = raw / 1000;
    uint32_t number = raw % 1000;
    char buf[20];
    snprintf(buf, sizeof(buf), "AEG%03u_%03u", group, number);
    return buf;
}

static uint32_t model_id_from_prefix(const std::string &prefix)
{
    // "AEG099_821" → 10099821
    if (prefix.size() < 10 || prefix.substr(0, 3) != "AEG" || prefix[6] != '_')
        return 0;
    unsigned group = 0, number = 0;
    if (sscanf(prefix.c_str(), "AEG%u_%u", &group, &number) != 2) return 0;
    return 10000000u + group * 1000u + number;
}

static std::set<uint64_t> g_collected_rows;
static int g_collected_count = 0;
static int g_unmatched_count = 0;

struct ParamRef {
    uint8_t *ptr;
    uint8_t original_areaNo;
};
static std::map<uint64_t, ParamRef> g_param_ptrs;

static std::map<uint32_t, std::vector<uint64_t>> g_tile_to_rows;                // tile → ordered row_ids
// tile → object_name → row_ids. A VECTOR because ERR duplicates part names when it
// copy-pastes assets (two AEG099_931_9006 in m12_02) — duplicate-named rows need
// per-position classification instead of the name-keyed fast path.
static std::map<uint32_t, std::map<std::string, std::vector<uint64_t>>> g_tile_name_to_row;
// 3D slot map: tile → prefix → geom_slot → row_ids (duplicate-named parts share the
// suffix-derived slot: the game writes one GEOF entry PER instance with the SAME slot
// value — verified live: two collected AEG099_931_9006 produced GEOF slots [6, 6]).
static std::map<uint32_t, std::map<std::string, std::map<int, std::vector<uint64_t>>>> g_tile_slot_to_row;
// MSB-local (posX, posY, posZ) per tracked row — used to detect ERR-style
// "replacement" where a different AEG099_* spawns at the same coords.
static std::map<uint64_t, std::tuple<float, float, float>> g_entry_positions;
static bool g_initialized = false;


struct GEOFEntry
{
    uint32_t tile_id;
    uint8_t flags;
    uint16_t geom_idx;
    uint32_t model_hash;  // bytes 4-7 of GEOF entry, identifies model type
};

// Each geom_idx holds two slots: flags=0x00 → even, flags=0x80 → odd
static int aeg099_index_from_geof(uint16_t geom_idx, uint8_t flags)
{
    return (geom_idx - GEOM_IDX_MIN) * 2 + ((flags & 0x80) ? 1 : 0);
}

// ─── GEOF from memory (GeomFlagSaveDataManager) ──────────────────────

// Singleton .data slots resolved by AOB (patch-resilient) instead of hardcoded
// RVAs: the game's static slots move on every update, so we pin them by a unique
// surrounding-code signature. relative_offsets {{3,7}} extracts the slot address
// from the `mov reg,[rip+slot]` xref; the AOB wildcards the rip-disp and branch
// targets so it survives patches. Resolved once, cached. (GeomNonActiveBlock-
// Manager is deliberately NOT read — see read_geof_from_memory and
// docs/geom_nonactive_block_manager.md.)
static uintptr_t resolve_slot(const char *aob)
{
    return reinterpret_cast<uintptr_t>(modutils::scan<void>({
        .aob = aob, .relative_offsets = {{3, 7}}}));
}
static uintptr_t geom_flag_slot()  // GeomFlagSaveDataManager (was RVA 0x3D69D18)
{
    static uintptr_t s = resolve_slot(goblin::sig::GEOM_FLAG_SLOT);
    return s;
}
static uintptr_t world_geom_man_slot()  // CSWorldGeomMan (was RVA 0x3D69BA8)
{
    static uintptr_t s = resolve_slot(
        goblin::sig::WORLD_GEOM_MAN_SLOT);
    return s;
}

static bool safe_read(void *addr, void *out, size_t count)
{
    // clang-cl ELIDES __try around a raw memcpy → use ReadProcessMemory (kernel call,
    // not elidable; returns false on a bad/freed addr). See goblin_worldmap_probe.
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, count, &got) && got == count;
}

// SEH-guarded single byte write. Returns true on success, false if the
// write access-violated (stale param pointer after another mod or the
// game relocated our buffer). Used per-pointer in refresh() so we can
// evict just the bad entries instead of tripping the top-level SEH and
// skipping the whole refresh cycle.
static bool safe_write_byte(uint8_t *addr, uint8_t val)
{
    // clang-cl ELIDES __try around a raw store → use WriteProcessMemory (kernel call,
    // not elidable; returns false on a bad/read-only addr). See goblin_worldmap_probe.
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), addr, &val, 1, &n) && n == 1;
}

// The +0x08 table is a Dantelion2 DLFixedVector (RE: windows_geom_flag_savedata_
// table_re_findings.md, commit 6d41b0e): inline contiguous, capacity 6300,
// element stride 0x10 { u32 tile_id; u32 pad; void* block }, sorted by tile_id,
// with the LIVE element count as a u64 at manager+0x189d0. So the valid elements
// are exactly [0, count) — no 0x20000/stride-16/256-empty heuristic needed.
//
// This collapses the per-refresh ReadProcessMemory count from ~16,000 (2 RPM per
// 16-byte slot across 128 KB) to 1 + 2·(#valid tiles) (~tens). On native Windows
// RPM-on-self is a cheap kernel path (the old scan showed ~0 lag); under Wine
// each RPM is a wineserver IPC round-trip (~10 µs), so the old scan cost ~153 ms
// (37% wallclock) on Linux/Proton — this is a Linux-only win, no-op on Windows.
// Block ptrs realloc on load/unload + the vector shifts on insert/evict, so a
// known-blocks cache is unsafe; a full bulk re-read each refresh is the correct
// cheap design (see findings §"Mutation model").
static constexpr uintptr_t GEOF_VEC_OFF = 0x08;       // inline DLFixedVector buffer
static constexpr uintptr_t GEOF_COUNT_OFF = 0x189d0;  // u64 element count
static constexpr uint64_t GEOF_CAPACITY = 6300;       // 0x189c fixed capacity

static void read_singleton_entries(uintptr_t slot,
                                    std::vector<GEOFEntry> &out)
{
    void *gf_ptr = nullptr;
    if (!slot || !safe_read((void *)slot, &gf_ptr, 8) || !gf_ptr)
        return;

    uint64_t count = 0;
    if (!safe_read((char *)gf_ptr + GEOF_COUNT_OFF, &count, 8))
        return;
    if (count == 0 || count > GEOF_CAPACITY)
        return; // empty or an implausible count → treat as nothing this refresh

    // One bulk RPM of the whole live region of the inline vector. On the rare
    // failure (a straddled unmapped page — unlikely for inline manager memory)
    // fall back to per-element 0x10 reads: still bounded by `count`, not 8192.
    std::vector<uint8_t> tbl(static_cast<size_t>(count) * 0x10);
    bool bulk_ok = safe_read((char *)gf_ptr + GEOF_VEC_OFF, tbl.data(), tbl.size());

    for (uint64_t i = 0; i < count; i++)
    {
        uint32_t tile_id = 0;
        uint64_t blk = 0;
        if (bulk_ok)
        {
            memcpy(&tile_id, tbl.data() + i * 0x10 + 0, 4);
            memcpy(&blk, tbl.data() + i * 0x10 + 8, 8);
        }
        else
        {
            // Per-element fallback (still O(count) RPM, not O(128KB/16)).
            uint8_t elem[0x10] = {};
            if (!safe_read((char *)gf_ptr + GEOF_VEC_OFF + i * 0x10, elem, 0x10))
                continue;
            memcpy(&tile_id, elem + 0, 4);
            memcpy(&blk, elem + 8, 8);
        }

        // Belt-and-suspenders (the vector is dense + sorted, so these rarely fire).
        uint8_t area = (tile_id >> 24) & 0xFF;
        if (area < 0x0A || area > 0x3D)
            continue;
        if (blk < 0x10000)
            continue;

        // Per-tile entry block (RE: the real format is the old "Layout A" —
        // count @+0x08, entries @+0x10 contiguous, stride 8). The "Layout B"
        // guess never fires; the engine only ever writes Layout A.
        uint8_t header[16] = {};
        if (!safe_read((void *)blk, header, 16))
            continue;
        uint32_t ecount = 0;
        memcpy(&ecount, header + 8, 4);
        if (ecount == 0 || ecount >= 100000)
            continue;

        // One bulk RPM of this tile's contiguous entry array.
        std::vector<uint8_t> ents(static_cast<size_t>(ecount) * 8);
        if (!safe_read((void *)(blk + 0x10), ents.data(), ents.size()))
            continue;

        for (uint32_t e = 0; e < ecount; e++)
        {
            const uint8_t *p = ents.data() + e * 8;
            // Decode unchanged + byte-correct (RE: record =
            // ((geom_idx | model_id<<17)<<15) | present → p[1] = (geom_idx&1)<<7,
            // hence the 0x00/0x80 filter; aeg099_index_from_geof reconstructs it).
            uint8_t entry_flags = p[1];
            uint16_t geom_idx = p[2] | (p[3] << 8);
            uint32_t model_hash = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);

            if (g_tracked_model_ids.count(model_hash) && (entry_flags == 0x00 || entry_flags == 0x80))
                out.push_back({tile_id, entry_flags, geom_idx, model_hash});
        }
    }
}

// ─── Read geom state from CSWorldGeomMan (loaded tiles) ─────────────
//
// Returns per-tile:
//   alive_names: set of ALIVE tracked-model names (for direct-name match)
//   occupied_positions: all AEG099_* instances at MSB-local coords, regardless
//     of name. Used to detect ERR-style "replacement" — e.g. a collected
//     AEG099_860 slot gets replaced by a respawning AEG099_780 at the same
//     position, so the original slot is effectively gone/collected.
//
// Name-based detection alone is not enough: the game spawns gathering-node
// CSWorldGeomIns lazily, so a truly-alive instance may simply not be loaded
// yet (and would be wrongly treated as dead by "not in alive list" logic).
struct WGMSnapshot
{
    std::set<std::string> alive_names;
    std::vector<std::tuple<float, float, float, std::string>> occupied;  // (x, y, z, name)
    // alive instances WITH positions — needed for duplicate-named parts (ERR copy-pastes
    // keep the part name, e.g. two AEG099_931_9006 in m12_02), where per-name state is
    // ambiguous and rows must be classified by the instance at THEIR coordinates.
    std::vector<std::tuple<float, float, std::string>> alive_occupied;  // (x, z, name)
};

// PERF cache for read_wgm_snapshot. The per-instance work (MSB-part pointer chain →
// wide name → family filter → 3 position floats) is the cost, and it grows unbounded as
// loaded tiles accumulate (CSWorldGeomMan never unloads them mid-session) → 108ms→1089ms.
// But that structure is STABLE per tile: only the two alive flags (+0x263/+0x26B) change
// between refreshes. So cache the resolved tracked-family instances per tile (keyed +
// validated by the tile's geom_ins vector begin/end pointers), and on a hit re-read ONLY
// the alive flags. Correctness is preserved: alive state is still read live every refresh.
struct WGMCacheInst
{
    void *geom_ins;
    float px, py, pz;
    std::string name;   // narrow AEG099_*/AEG463_* part name
    bool track_alive;   // prefix is in g_tracked_prefixes → read alive flags
};
struct WGMCacheTile
{
    void *vec_begin = nullptr, *vec_end = nullptr;
    std::vector<WGMCacheInst> insts;   // only tracked-FAMILY instances (structure)
};
static std::map<uint32_t, WGMCacheTile> g_wgm_cache;

// Build the per-tile snapshot from a cached instance list: occupancy from the cached
// structure, alive state re-read live (the only thing that changes between refreshes).
static void wgm_snap_from_cache(const WGMCacheTile &ct, WGMSnapshot &snap)
{
    for (const WGMCacheInst &ci : ct.insts)
    {
        snap.occupied.emplace_back(ci.px, ci.py, ci.pz, ci.name);
        if (!ci.track_alive) continue;
        // The two alive flags are at +0x263 / +0x26B → read the 9-byte span in ONE RPM
        // instead of two (halves the per-instance warm-path cost — the recurring read_wgm
        // tax under Wine, where every RPM is a wineserver round-trip). f263 = bits[0],
        // f26B = bits[8]; decode unchanged.
        uint8_t fl[9] = {};
        if (!safe_read((char *)ci.geom_ins + 0x263, fl, 9))
            continue;
        uint8_t f263 = fl[0], f26B = fl[8];
        if ((f263 & 0x02) && !(f26B & 0x10))
        {
            snap.alive_names.insert(ci.name);
            snap.alive_occupied.emplace_back(ci.px, ci.pz, ci.name);
        }
    }
}

static std::map<uint32_t, WGMSnapshot> read_wgm_snapshot()
{
    std::map<uint32_t, WGMSnapshot> result;
    std::set<uint32_t> seen_tiles;   // for pruning unloaded tiles from the cache
    static int s_fieldins_logged = 0;   // [FIELDINS] path-A probe one-shot budget

    uintptr_t game_base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    if (!game_base) return result;

    void *wgm = nullptr;
    if (!safe_read((void *)world_geom_man_slot(), &wgm, 8) || !wgm)
        return result;

    // Tree at WGM+0x18: +0x08 head_ptr, +0x10 size
    void *tree_head = nullptr;
    uint64_t tree_size = 0;
    if (!safe_read((char *)wgm + 0x18 + 0x08, &tree_head, 8) || !tree_head)
        return result;
    safe_read((char *)wgm + 0x18 + 0x10, &tree_size, 8);

    if (tree_size == 0 || tree_size > 1000)
        return result;

    // RB tree node: +0x00 left, +0x08 parent, +0x10 right, +0x19 is_nil, +0x20 value
    // Value (BlocksEntry): +0x00 block_id(u32), +0x08 data_ptr
    void *root = nullptr;
    safe_read((char *)tree_head + 0x08, &root, 8); // parent of head = root

    auto get_is_nil = [](void *node) -> bool {
        uint8_t val = 1;
        safe_read((char *)node + 0x19, &val, 1);
        return val != 0;
    };

    auto get_left = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x00, &p, 8);
        return p;
    };

    auto get_right = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x10, &p, 8);
        return p;
    };

    auto get_parent = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x08, &p, 8);
        return p;
    };

    auto min_node = [&](void *node) -> void * {
        while (node && !get_is_nil(node)) {
            void *left = get_left(node);
            if (!left || get_is_nil(left)) break;
            node = left;
        }
        return node;
    };

    void *current = min_node(root);
    int nodes_visited = 0;

    while (current && current != tree_head && !get_is_nil(current) && nodes_visited < 500)
    {
        nodes_visited++;

        uint32_t block_id = 0;
        void *block_data = nullptr;
        safe_read((char *)current + 0x20, &block_id, 4);
        safe_read((char *)current + 0x28, &block_data, 8);

        if (block_data)
        {
            // geom_ins vector at BlockData+0x288
            void *vec_begin = nullptr, *vec_end = nullptr;
            safe_read((char *)block_data + 0x288 + 0x08, &vec_begin, 8);
            safe_read((char *)block_data + 0x288 + 0x10, &vec_end, 8);

            seen_tiles.insert(block_id);

            // CACHE HIT: same tile vector as last walk → reuse resolved structure, only
            // re-read alive flags (done in wgm_snap_from_cache). Skips the expensive
            // per-instance name/position resolution.
            auto cit = g_wgm_cache.find(block_id);
            bool cache_hit = (cit != g_wgm_cache.end() && cit->second.vec_begin == vec_begin &&
                              cit->second.vec_end == vec_end && vec_begin);
            if (cache_hit)
            {
                if (!cit->second.insts.empty())
                    wgm_snap_from_cache(cit->second, result[block_id]);
            }
            // CACHE MISS: full resolve + rebuild this tile's cache entry.
            else if (true)
            {
            WGMCacheTile ctile;
            ctile.vec_begin = vec_begin;
            ctile.vec_end = vec_end;

            if (vec_begin && vec_end && vec_end > vec_begin)
            {
                size_t count = ((uintptr_t)vec_end - (uintptr_t)vec_begin) / 8;
                if (count > 10000) count = 10000;

                // Bulk-read the whole contiguous geom_ins POINTER vector in ONE RPM
                // (instead of one RPM per element). Under Wine each RPM is a wineserver
                // round-trip, so this cache-MISS path (the felt ~960ms freeze on tile
                // load) is dominated by raw call count. Fallback: per-element on failure.
                std::vector<void *> ins_ptrs(count, nullptr);
                bool vec_bulk = safe_read(vec_begin, ins_ptrs.data(), count * 8);

                for (size_t i = 0; i < count; i++)
                {
                    void *geom_ins = nullptr;
                    if (vec_bulk)
                        geom_ins = ins_ptrs[i];
                    else
                        safe_read((char *)vec_begin + i * 8, &geom_ins, 8);
                    if (!geom_ins) continue;

                    // Bulk the geom_ins header (0..0x26C) in ONE RPM: covers the MSB-part
                    // pointer (+0x48 = +0x18*3) and BOTH alive flags (+0x263 / +0x26B),
                    // replacing 3 separate reads. geom_ins objects are >0x270, so this
                    // never straddles past the alloc in practice; skip on a faulted read.
                    uint8_t gi[0x26C] = {};
                    if (!safe_read(geom_ins, gi, sizeof(gi))) continue;
                    void *msb_part_ptr = nullptr;
                    memcpy(&msb_part_ptr, gi + 0x18 + 0x18 + 0x18, 8);
                    if (!msb_part_ptr) continue;

                    // Bulk the MSB-part header (0..0x2C) in ONE RPM: the name pointer
                    // (+0x00) and the runtime position (+0x20, 3 floats).
                    uint8_t mp[0x2C] = {};
                    if (!safe_read(msb_part_ptr, mp, sizeof(mp))) continue;
                    void *name_ptr = nullptr;
                    memcpy(&name_ptr, mp + 0, 8);
                    if (!name_ptr) continue;

                    wchar_t name_buf[64] = {};
                    safe_read(name_ptr, name_buf, sizeof(name_buf) - 2);

                    char narrow[64] = {};
                    for (int c = 0; c < 63 && name_buf[c]; c++)
                        narrow[c] = (char)(name_buf[c] & 0xFF);

                    // Keep only tracked AEG families. ERR uses both AEG099_*
                    // (base-game gathering assets) and AEG463_* (DLC flowers/
                    // bodies). Everything else (AEG001 decorations, colliders,
                    // signs, …) is noise — drop it early.
                    std::string narrow_str(narrow);
                    bool is_tracked_family =
                        narrow_str.compare(0, 7, "AEG099_") == 0 ||
                        narrow_str.compare(0, 7, "AEG463_") == 0;
                    if (!is_tracked_family)
                        continue;

                    // Runtime position lives in MsbPart +0x20 (3 floats) — already in the
                    // bulked MSB-part blob, no extra RPM.
                    float px = 0, py = 0, pz = 0;
                    memcpy(&px, mp + 0x20 + 0, 4);
                    memcpy(&py, mp + 0x20 + 4, 4);
                    memcpy(&pz, mp + 0x20 + 8, 4);

                    auto &snap = result[block_id];

                    // Record occupancy for position-based replacement detection
                    snap.occupied.emplace_back(px, py, pz, narrow_str);

                    // Track alive state only for models we're actually hiding on the map
                    std::string prefix = prefix_from_object_name(narrow);
                    bool track_alive = !prefix.empty() && g_tracked_prefixes.count(prefix) != 0;
                    if (track_alive)
                    {
                        // +0x263 bit1: persistent alive flag (survives restart)
                        // +0x26B bit4: immediate collection flag (lost on tile reload)
                        // Both already in the bulked geom_ins blob (gi), no extra RPM.
                        uint8_t f263 = gi[0x263], f26B = gi[0x26B];

                        bool alive = (f263 & 0x02) && !(f26B & 0x10);
                        if (alive)
                        {
                            snap.alive_names.insert(narrow_str);
                            snap.alive_occupied.emplace_back(px, pz, narrow_str);
                        }
                    }

                    // Cache the resolved instance (structure) for next refresh's fast path.
                    ctile.insts.push_back({geom_ins, px, py, pz, narrow_str, track_alive});

                    // [FIELDINS] path-A asset→ItemLotID join probe (diag_fieldins_join, one-shot).
                    // Read the CSGrowableNodePool embedded in this asset instance at geom_ins+0x3A8
                    // (RE be1b018): cap@+0x3B8, stride@+0x3BC, node-array@+0x3C0; the pooled child is
                    // a FieldInsBase* carrying lotId@+0x50 + inline wide name. Expect, on the known
                    // chest AEG099_090_9000, lotId == 0x3dd6fec4 (1037500100). One bulk read.
                    if (goblin::config::diagFieldinsJoin && s_fieldins_logged < 24)
                    {
                        uint8_t pool[0x20] = {};   // geom_ins+0x3A8 .. +0x3C8
                        if (safe_read((char *)geom_ins + 0x3A8, pool, sizeof(pool)))
                        {
                            void *pool_vt = nullptr, *node_arr = nullptr;
                            uint32_t cap = 0, stride = 0;
                            memcpy(&pool_vt,  pool + 0x00, 8);   // +0x3A8
                            memcpy(&cap,      pool + 0x10, 4);   // +0x3B8
                            memcpy(&stride,   pool + 0x14, 4);   // +0x3BC
                            memcpy(&node_arr, pool + 0x18, 8);   // +0x3C0
                            uint32_t lot = 0;
                            char child_name[64] = {};
                            bool got_child = false;
                            if (node_arr)
                            {
                                void *child = nullptr;            // node[0] = FieldInsBase*
                                if (safe_read(node_arr, &child, 8) && child)
                                {
                                    got_child = safe_read((char *)child + 0x50, &lot, 4);
                                    wchar_t cn[32] = {};
                                    if (safe_read(child, cn, sizeof(cn) - 2))
                                        for (int c = 0; c < 31 && cn[c]; c++)
                                            child_name[c] = (char)(cn[c] & 0xFF);
                                }
                            }
                            spdlog::info("[FIELDINS] {} pool_vt={} cap={} stride={} node_arr={} "
                                         "child={} lotId={:#x} name='{}'",
                                         narrow_str, pool_vt, cap, stride, node_arr,
                                         got_child ? "yes" : "no", lot, child_name);
                            ++s_fieldins_logged;
                        }
                    }
                }
            }

            g_wgm_cache[block_id] = std::move(ctile); // (re)build this tile's cache entry
            } // end CACHE MISS
        }

        // In-order successor
        void *right = get_right(current);
        if (right && !get_is_nil(right))
        {
            current = min_node(right);
        }
        else
        {
            void *parent = get_parent(current);
            int walk_up = 0;
            while (parent && parent != tree_head && ++walk_up < 500)
            {
                void *parent_right = get_right(parent);
                if (current != parent_right) break;
                current = parent;
                parent = get_parent(current);
            }
            if (walk_up >= 500) break;
            current = parent;
        }
    }

    // Prune cache entries for tiles no longer loaded (bounds memory across a session).
    for (auto it = g_wgm_cache.begin(); it != g_wgm_cache.end();)
        it = seen_tiles.count(it->first) ? std::next(it) : g_wgm_cache.erase(it);

    // [FIELDINS-B] one-shot global field-instance registry walk (diag_fieldins_join).
    // CORRECTED chain (Ghidra da19285): reg=[er+0x3d7b0c0], sub=[reg+0x10],
    // container=*[sub+0x720] (the prior walk MISSED this deref). The self-register map is at
    // container+0x18 (std::map: _Myhead @+0x08 → head=*[container+0x20], _Mysize @+0x28).
    // RB node: +0x00 left / +0x08 parent / +0x10 right / +0x19 _Isnil / +0x20 key(u64);
    // value is 24B → +0x28 is a per-instance CALLBACK fn ptr, the INSTANCE is at +0x30 (the
    // prior probe read +0x28 → got a code ptr 0x30308e8). Instance: vtable@+0x00, lotId@+0x50.
    // No dedicated treasure class — lotId carriers are geom-items (CSWorldGeom*Ins); sealed
    // chests spawn their pickup only at open, placed/dropped world loot is resident at load.
    // GATED on !result.empty() (in a save, not the menu); latches only after a real in-world walk.
    static bool s_pathb_done = false;
    if (goblin::config::diagFieldinsJoin && !s_pathb_done && !result.empty())
    {
        void *reg = nullptr, *sub = nullptr, *container = nullptr, *head = nullptr, *root = nullptr;
        uint64_t map_size = 0;
        if (safe_read((void *)(game_base + 0x3d7b0c0), &reg, 8) && reg &&
            safe_read((char *)reg + 0x10, &sub, 8) && sub &&
            safe_read((char *)sub + 0x720, &container, 8) && container &&
            safe_read((char *)container + 0x20, &head, 8) && head &&
            safe_read((char *)container + 0x28, &map_size, 8) &&
            safe_read((char *)head + 0x08, &root, 8) && root && root != head)
        {
            const int kMaxNodes = 400000;
            int visited = 0, instances = 0, lot_hits = 0, real_lot = 0;
            std::vector<void *> stack;
            std::unordered_set<void *> seen;          // kill cycles / garbage revisits
            std::unordered_set<void *> sampled_inst;  // dedup the sample lines
            std::map<uint64_t, int> vt_hist;          // distinct vtable → count (for real lots)
            stack.push_back(root);
            while (!stack.empty() && visited < kMaxNodes)
            {
                void *node = stack.back(); stack.pop_back();
                if (!node || node == head) continue;
                if (!seen.insert(node).second) continue;   // already walked this node
                uint8_t nb[0x38] = {};   // need +0x30 (instance ptr)
                if (!safe_read(node, nb, sizeof(nb))) continue;
                ++visited;
                void *left = nullptr, *right = nullptr, *inst = nullptr;
                memcpy(&left,  nb + 0x00, 8);
                memcpy(&right, nb + 0x10, 8);
                memcpy(&inst,  nb + 0x30, 8);   // +0x28 = callback fn ptr; instance is +0x30
                if (right && right != head) stack.push_back(right);
                if (left  && left  != head) stack.push_back(left);
                if (!inst) continue;
                ++instances;
                uint8_t ib[0x58] = {};   // vtable@+0x00 + lotId@+0x50
                if (!safe_read(inst, ib, sizeof(ib))) continue;
                uint64_t vt = 0; uint32_t lot = 0;
                memcpy(&vt,  ib + 0x00, 8);
                memcpy(&lot, ib + 0x50, 4);
                if (lot == 0x3dd6fec4) ++lot_hits;
                // Real ER item-lots are ~1e9 (0x3B9ACA00..); 0 and 0xffffffff are "no lot".
                bool plausible = lot != 0 && lot != 0xffffffff && lot >= 0x10000000;
                if (!plausible) continue;
                ++real_lot;
                vt_hist[vt]++;
                if (sampled_inst.insert(inst).second && sampled_inst.size() <= 24)
                    spdlog::info("[FIELDINS-B] inst={} vt={:#x} lotId={:#x}", inst, vt, lot);
            }
            s_pathb_done = true;   // latch only after a real in-world walk
            spdlog::info("[FIELDINS-B] DONE mapSize={} visited={} distinctNodes={} instances={} "
                         "realLot={} targetLotHits={} (target=0x3dd6fec4)",
                         (unsigned long long)map_size, visited, (int)seen.size(),
                         instances, real_lot, lot_hits);
            for (auto &[vt, n] : vt_hist)
                spdlog::info("[FIELDINS-B] vtable={:#x} realLotInstances={}", vt, n);
        }
        else
        {
            spdlog::info("[FIELDINS-B] registry chain unresolved (reg={} sub={} head={} root={})",
                         reg, sub, head, root);
        }
    }

    return result;
}

static std::vector<GEOFEntry> read_geof_from_memory()
{
    std::vector<GEOFEntry> result;

    uintptr_t game_base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    if (!game_base)
        return result;

    read_singleton_entries(geom_flag_slot(), result);

    // NOTE: GeomNonActiveBlockManager (RVA_GEOM_NONACTIVE) is intentionally NOT
    // scanned. Despite the name, its layout is nothing like GeomFlagSaveData-
    // Manager — it is a 0x820-byte object holding a fixed inline array of 0x20-
    // byte block records (count at +0x818, active flag at +0x08), not a
    // (tile_id, ptr) table at +0x08. read_singleton_entries assumes the GeomFlag
    // layout, so applied here it walked ~126 KB past the object end into
    // unrelated heap, raising a stream of (safe_read-caught) access violations
    // and never returning valid data on any game version. Collected-geometry
    // state comes from GeomFlagSaveDataManager (above) + CSWorldGeomMan (loaded
    // tiles) + the immediate per-AEG +0x26B flag. Full RE:
    // docs/geom_nonactive_block_manager.md.

    if (!result.empty())
        spdlog::debug("[GEOF] Memory: {} flag-save entries", result.size());

    return result;
}

// ─── tile ID helper ──────────────────────────────────────────────────

static uint32_t encode_tile(uint8_t area, uint8_t gridX, uint8_t gridZ)
{
    return ((uint32_t)area << 24) | ((uint32_t)gridX << 16) | ((uint32_t)gridZ << 8);
}

// ─── main initialization ─────────────────────────────────────────────

void goblin::collected::initialize()
{
    GOBLIN_BENCH("init.collected_initialize");
    g_collected_rows.clear();
    g_collected_count = 0;
    g_unmatched_count = 0;

    g_tile_to_rows.clear();
    g_tile_slot_to_row.clear();
    g_tile_name_to_row.clear();
    g_tracked_prefixes.clear();
    g_tracked_model_ids.clear();
    g_entry_positions.clear();

    // Build tracking tables from MAP_ENTRIES — any entry with object_name is tracked.
    // Adding a new model type only requires adding entries + _slots.json.
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        if (!e.object_name || !e.object_name[0])
            continue;

        std::string prefix = prefix_from_object_name(e.object_name);
        if (prefix.empty())
            continue;

        g_tracked_prefixes.insert(prefix);

        // GEOF buckets must use the part's ACTUAL model, which ERR sometimes substitutes
        // (part AEG099_753_9000 instantiating DLC model AEG463_860): the game writes GEOF
        // entries under the actual model's hash, so matching by the NAME prefix misses
        // them and collected flowers stay visible on unloaded tiles after a restart.
        std::string geof_prefix = prefix;
        {
            auto *end = generated::GEOF_MODEL_OVERRIDES + generated::GEOF_MODEL_OVERRIDE_COUNT;
            auto *ov = std::lower_bound(
                generated::GEOF_MODEL_OVERRIDES, end, e.row_id,
                [](const generated::GeofModelOverride &o, uint64_t id) { return o.row_id < id; });
            if (ov != end && ov->row_id == e.row_id)
            {
                geof_prefix = prefix_from_model_id(ov->model_id);
                g_tracked_prefixes.insert(geof_prefix);  // feeds g_tracked_model_ids (GEOF filter)
            }
        }

        uint32_t tile = encode_tile(e.data.areaNo, e.data.gridXNo, e.data.gridZNo);
        g_tile_to_rows[tile].push_back(e.row_id);

        // 3D slot map: tile → ACTUAL-model prefix → geom_slot → row_ids
        if (e.geom_slot >= 0)
            g_tile_slot_to_row[tile][geof_prefix][e.geom_slot].push_back(e.row_id);

        // WGM name tracking: full object name for accurate alive matching
        g_tile_name_to_row[tile][e.object_name].push_back(e.row_id);

        // MSB-local position for replacement detection via WGM occupancy
        g_entry_positions[e.row_id] = {e.data.posX, e.data.posY, e.data.posZ};
    }

    // Build model_id set for GEOF filtering
    for (auto &prefix : g_tracked_prefixes)
    {
        uint32_t mid = model_id_from_prefix(prefix);
        if (mid)
            g_tracked_model_ids.insert(mid);
    }

    int total_piece_entries = 0;
    for (auto &[t, rows] : g_tile_to_rows)
        total_piece_entries += (int)rows.size();

    spdlog::info("[COLLECTED] {} entries across {} tiles, {} tracked prefixes ({} model IDs)",
                 total_piece_entries, g_tile_to_rows.size(),
                 g_tracked_prefixes.size(), g_tracked_model_ids.size());

    for (auto &prefix : g_tracked_prefixes)
        spdlog::debug("[COLLECTED]   prefix: {} -> model_id {}", prefix, model_id_from_prefix(prefix));

    g_initialized = true;
    spdlog::info("[COLLECTED] Initialized, awaiting refresh()");
}

// ─── remap row IDs after dynamic assignment ────────────────────────

static std::unordered_map<uint64_t, uint64_t> g_original_to_dynamic;

void goblin::collected::remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new)
{
    g_original_to_dynamic = old_to_new;

    // Remap g_tile_to_rows
    for (auto &[tile, rows] : g_tile_to_rows)
    {
        for (auto &rid : rows)
        {
            auto it = old_to_new.find(rid);
            if (it != old_to_new.end())
                rid = it->second;
        }
    }

    // Remap g_tile_slot_to_row (3D: tile → prefix → slot → row_ids)
    for (auto &[tile, prefix_map] : g_tile_slot_to_row)
    {
        for (auto &[prefix, slot_map] : prefix_map)
        {
            for (auto &[slot, rids] : slot_map)
            {
                for (auto &rid : rids)
                {
                    auto it = old_to_new.find(rid);
                    if (it != old_to_new.end())
                        rid = it->second;
                }
            }
        }
    }

    // Remap g_tile_name_to_row
    for (auto &[tile, name_map] : g_tile_name_to_row)
    {
        for (auto &[obj_name, rids] : name_map)
        {
            for (auto &rid : rids)
            {
                auto it = old_to_new.find(rid);
                if (it != old_to_new.end())
                    rid = it->second;
            }
        }
    }

    // Remap g_entry_positions (keyed by row_id)
    decltype(g_entry_positions) remapped;
    for (auto &[rid, pos] : g_entry_positions)
    {
        auto it = old_to_new.find(rid);
        uint64_t new_rid = (it != old_to_new.end()) ? it->second : rid;
        remapped[new_rid] = pos;
    }
    g_entry_positions = std::move(remapped);

    spdlog::debug("[COLLECTED] Remapped {} row IDs", old_to_new.size());
}

// ─── hide icon in-place ─────────────────────────────────────────────

static void hide_icon(void *param_data)
{
    if (!param_data) return;
    auto *p = reinterpret_cast<uint8_t *>(param_data);
    // Set areaNo (offset 0x20, uint8) to 99 → non-existent area, icon won't display
    p[0x20] = 99;
}

// ─── register param pointer for real-time hiding ────────────────────

void goblin::collected::register_param_ptr(uint64_t row_id, void *param_data)
{
    auto *p = reinterpret_cast<uint8_t *>(param_data);
    g_param_ptrs[row_id] = {p, p[0x20]};  // save original areaNo
}

bool goblin::collected::is_registered(uint64_t row_id)
{
    return g_param_ptrs.count(row_id) > 0;
}

// ─── refresh from memory (real-time update) ─────────────────────────

int goblin::collected::refresh()
{
    uintptr_t game_base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    if (!game_base)
        return 0;
    void *wgm_check = nullptr;
    safe_read((void *)world_geom_man_slot(), &wgm_check, 8);
    void *geof_check = nullptr;
    safe_read((void *)geom_flag_slot(), &geof_check, 8);
    if (!wgm_check && !geof_check)
        return 0;

    if (!g_initialized || g_tile_to_rows.empty())
        return 0;

    GOBLIN_BENCH("refresh.collected.total");

    std::vector<GEOFEntry> geof;
    {
        GOBLIN_BENCH("refresh.collected.read_geof");
        geof = read_geof_from_memory();
    }

    std::set<uint64_t> new_collected;

    // Group GEOF entries by tile + prefix
    // key: tile_id → prefix → list of GEOF-computed slots
    std::map<uint32_t, std::map<std::string, std::vector<int>>> geof_tile_prefix_slots;
    for (auto &e : geof)
    {
        if (g_tile_to_rows.find(e.tile_id) == g_tile_to_rows.end())
            continue;
        std::string prefix = prefix_from_model_id(e.model_hash);
        if (prefix.empty())
            continue;
        int slot = aeg099_index_from_geof(e.geom_idx, e.flags);
        geof_tile_prefix_slots[e.tile_id][prefix].push_back(slot);
    }

    // ── WGM: tracking for loaded tiles ──
    // A row is collected if EITHER:
    //   (A) Its MSB-part name is NOT present as alive in WGM, AND a different
    //       AEG099_* instance occupies the same MSB-local position (ERR-style
    //       replacement: collected smithing stone becomes a respawning
    //       cracked-crystal of a different model at the same coords).
    //   (B) No replacement at the position, but we at least KNOW the row's
    //       own instance is loaded and dead (name directly present but not in
    //       alive_names).
    //
    // We CANNOT simply say "name not in alive_names → collected" because the
    // game spawns alive gathering-node CSWorldGeomIns lazily — an absent name
    // may just mean "not yet spawned near the player".
    std::map<uint32_t, WGMSnapshot> wgm;
    {
        GOBLIN_BENCH("refresh.collected.read_wgm");
        wgm = read_wgm_snapshot();
    }
    std::set<uint32_t> wgm_tiles;

    // [LOOTPOS] one-shot in-process placement-accuracy probe (diag_loot_pos): for every loaded
    // MSB asset this walk sees, compare its LIVE MsbPart position (px/pz @ MsbPart+0x20, read by
    // the SHIPPING collected walk) against the baked MAP_ENTRY placement (g_entry_positions, same
    // MSB-local frame). Proves the data we collect matches the bake from INSIDE the process —
    // stronger than the external RPM script that re-implements the walk. One-shot per session;
    // needs loot loaded near the player. X/Z only (baked posY often 0, see pos_key below).
    static bool s_lootpos_done = false;
    if (goblin::config::diagLootPos && !s_lootpos_done)
    {
        int compared = 0, within = 0, missing_baked = 0;
        float max_d = 0.f;
        for (auto &[tile_id, snap] : wgm)
        {
            auto name_it = g_tile_name_to_row.find(tile_id);
            if (name_it == g_tile_name_to_row.end()) continue;
            for (auto &[lx, ly, lz, lname] : snap.occupied)
            {
                (void)ly;
                auto rit = name_it->second.find(lname);
                if (rit == name_it->second.end()) continue;
                for (uint64_t row_id : rit->second)
                {
                    auto pit = g_entry_positions.find(row_id);
                    if (pit == g_entry_positions.end()) { ++missing_baked; continue; }
                    auto [bx, by, bz] = pit->second;
                    (void)by;
                    float dx = lx - bx, dz = lz - bz;
                    float d = std::sqrt(dx * dx + dz * dz);
                    ++compared;
                    if (d <= 0.5f) ++within;
                    if (d > max_d) max_d = d;
                    if (d > 0.5f)
                        spdlog::info("[LOOTPOS] tile={:#x} {} row={} baked=({:.2f},{:.2f}) "
                                     "live=({:.2f},{:.2f}) dXZ={:.2f}",
                                     tile_id, lname, row_id, bx, bz, lx, lz, d);
                }
            }
        }
        if (compared > 0)   // only latch once we actually saw loaded loot
        {
            s_lootpos_done = true;
            spdlog::info("[LOOTPOS] DONE compared={} within0.5={} ({:.1f}%) maxDXZ={:.2f} missingBaked={}",
                         compared, within, 100.0 * within / compared, max_d, missing_baked);
        }
    }

    // Rows we positively observed alive in WGM this refresh. Used to override
    // sticky carry-forward below: a row is "uncollected" only when we see its
    // live instance, not just because the tile is unloaded.
    std::set<uint64_t> demonstrably_alive_rows;

    // Position key uses X/Z only (rounded to int). posY in MAP_ENTRIES is sometimes
    // not set (defaults to 0) while WGM's posY is the real MSB height — matching on
    // Y would cause false negatives. Same-XZ collisions are extremely rare in
    // practice (gathering nodes don't stack vertically).
    auto pos_key = [](float x, float z) {
        return std::make_pair((int)std::lround(x), (int)std::lround(z));
    };

    for (auto &[tile_id, snap] : wgm)
    {
        wgm_tiles.insert(tile_id);

        auto name_it = g_tile_name_to_row.find(tile_id);
        if (name_it == g_tile_name_to_row.end())
            continue;

        std::map<std::pair<int, int>, std::vector<std::string>> pos_to_names;
        for (auto &[x, y, z, name] : snap.occupied)
            pos_to_names[pos_key(x, z)].push_back(name);
        std::map<std::pair<int, int>, std::set<std::string>> alive_pos_to_names;
        for (auto &[x, z, name] : snap.alive_occupied)
            alive_pos_to_names[pos_key(x, z)].insert(name);

        // Walk every tracked MAP_ENTRY on this tile and classify it.
        for (auto &[object_name, row_ids] : name_it->second)
        {
            // Fast path: unique name (the normal case). Its own instance alive → visible.
            if (row_ids.size() == 1 && snap.alive_names.count(object_name))
            {
                demonstrably_alive_rows.insert(row_ids.front());
                continue;
            }

            for (uint64_t row_id : row_ids)
            {
                // Look up MAP_ENTRY coords to probe the position index
                auto pt_it = g_entry_positions.find(row_id);
                if (pt_it == g_entry_positions.end()) continue;
                auto [ex, ey, ez] = pt_it->second;

                // Check a 3x3 integer-bucket neighborhood. Strict single-key lookup
                // misses cases where an adjacent asset sits on the border of the
                // rounding boundary — e.g. AEG463_600 at X=-60.48 rounds to -60
                // while the gathered AEG463_840 at X=-60.96 probes key -61.
                int cx = (int)std::lround(ex);
                int cz = (int)std::lround(ez);

                // Duplicate-named parts (ERR copy-pastes keep the part name): the
                // name-level alive check is ambiguous — classify THIS row by the
                // instance standing at the row's own coordinates.
                if (row_ids.size() > 1)
                {
                    bool alive_here = false;
                    for (int dx = -1; dx <= 1 && !alive_here; ++dx)
                        for (int dz = -1; dz <= 1 && !alive_here; ++dz)
                        {
                            auto ap = alive_pos_to_names.find({cx + dx, cz + dz});
                            if (ap != alive_pos_to_names.end() && ap->second.count(object_name))
                                alive_here = true;
                        }
                    if (alive_here)
                    {
                        demonstrably_alive_rows.insert(row_id);
                        continue;
                    }
                }

                bool occupied = false;
                for (int dx = -1; dx <= 1 && !occupied; ++dx)
                    for (int dz = -1; dz <= 1 && !occupied; ++dz)
                        if (pos_to_names.count({cx + dx, cz + dz}))
                            occupied = true;
                if (!occupied) continue;  // nothing nearby → undetermined, keep visible

                // Something is there. Either our own dead instance, or a replacement
                // spawned by ERR (collected AEG099_860 → respawning AEG099_780 at the
                // same coords, or AEG463_600 body where the AEG463_840 flower stood).
                new_collected.insert(row_id);
            }
        }
    }

    // ── GEOF: for unloaded tiles ──
    for (auto &[tid, prefix_slots] : geof_tile_prefix_slots)
    {
        if (wgm_tiles.count(tid))
            continue;

        auto tile_it = g_tile_slot_to_row.find(tid);
        if (tile_it == g_tile_slot_to_row.end()) continue;

        for (auto &[prefix, slots] : prefix_slots)
        {
            auto prefix_it = tile_it->second.find(prefix);
            if (prefix_it == tile_it->second.end()) continue;

            // Single-instance fallback. If this tile has exactly one row for
            // this prefix, any GEOF entry means that row was collected — no
            // need to match a slot. aeg099_index_from_geof() is calibrated
            // for AEG099_*; AEG463_* uses a different encoding (verified via
            // memory dump: m60_48_36 AEG463_840 has geom_slot=5 in MAP_ENTRY
            // but GEOF reports geom_idx=4503 → AEG099 formula computes
            // slot=6, off by one). For single-instance prefixes the slot is
            // irrelevant, so skip the lookup entirely.
            if (prefix_it->second.size() == 1 && prefix_it->second.begin()->second.size() == 1)
            {
                new_collected.insert(prefix_it->second.begin()->second.front());
                continue;
            }

            // Per-slot match. Duplicate-named parts (ERR copy-pastes that keep the
            // part Name) collapse to the SAME (model_id, geom_idx) key in the engine —
            // their GEOF records are byte-identical and the slot is shared. The save/
            // load path (eldenring.exe 0x6b2b80, RE'd 2026-06) keys collected state
            // ONLY on (model_id, geom_idx): on tile reload the engine marks EVERY
            // instance with a matching key collected — all-or-nothing per slot, twins
            // are fused and one becomes un-collectable as a separate object. So any
            // GEOF entry for a slot ⇒ hide ALL rows mapped to that slot (matches the
            // engine; the loaded-tile WGM path above still distinguishes twins live).
            for (int slot : slots)
            {
                auto row_it = prefix_it->second.find(slot);
                if (row_it == prefix_it->second.end()) continue;
                for (uint64_t r : row_it->second)
                    new_collected.insert(r);
            }
        }
    }

    // ── Sticky carry-forward ──
    //
    // For models that don't leave a replacement asset and aren't tracked by
    // GeomFlagSaveDataManager (notably AEG463_840 "Dragon's Calorbloom" with
    // isBreakOnPickUp=True), both WGM-replacement and GEOF detection lose
    // track after the player walks far from the tile — WGM goes empty and
    // GEOF has no entry. Without carry-forward the row drops back to
    // "uncollected" and the icon reappears on the map.
    //
    // Rule: if a row was collected last refresh and we don't positively see
    // its live instance now, it stays collected. Respawn (e.g. after grace
    // rest, when the tile reloads with the flower alive) puts the row into
    // demonstrably_alive_rows and breaks the sticky retention.
    int carried = 0;
    for (uint64_t prev : g_collected_rows)
    {
        if (demonstrably_alive_rows.count(prev))
            continue;  // respawn observed → release
        if (new_collected.insert(prev).second)
            carried++;
    }
    if (carried > 0)
        spdlog::debug("[COLLECTED] Sticky carry-forward kept {} row(s) hidden", carried);

    if (new_collected == g_collected_rows)
        return 0;
    // Log which rows were added/removed (for small deltas only)
    {
        std::vector<uint64_t> added, removed;
        for (auto r : new_collected)
            if (!g_collected_rows.count(r)) added.push_back(r);
        for (auto r : g_collected_rows)
            if (!new_collected.count(r)) removed.push_back(r);
        spdlog::info("[COLLECTED] Set changed: {} -> {} (added {}, removed {})",
                     g_collected_rows.size(), new_collected.size(), added.size(), removed.size());
        if (added.size() <= 10)
            for (auto r : added) spdlog::info("[COLLECTED]   +row {}", r);
        if (removed.size() <= 10)
            for (auto r : removed) spdlog::info("[COLLECTED]   -row {}", r);
    }

    // Restore all to visible, then re-hide collected.
    // Each write is SEH-guarded individually so a single stale pointer
    // (e.g. from another mod or game reloading the param file) gets
    // evicted instead of tripping the top-level SEH and skipping the
    // entire refresh cycle until the game is restarted.
    std::vector<uint64_t> stale;
    for (auto &[row_id, ref] : g_param_ptrs)
    {
        // Respect a section toggle that is keeping this row hidden — otherwise
        // the restore-all would un-hide it (the section-vs-collected areaNo
        // conflict).
        uint8_t target = goblin::is_section_hidden_ptr(ref.ptr) ? 99 : ref.original_areaNo;
        if (!safe_write_byte(ref.ptr + 0x20, target))
            stale.push_back(row_id);
    }

    int applied = 0, missed = 0;
    for (uint64_t row_id : new_collected)
    {
        auto pit = g_param_ptrs.find(row_id);
        if (pit != g_param_ptrs.end())
        {
            if (safe_write_byte(pit->second.ptr + 0x20, 99))
                applied++;
            else
                stale.push_back(row_id);
        }
        else
            missed++;
    }

    if (!stale.empty())
    {
        // Dedup and evict — these pointers are no longer writable.
        std::sort(stale.begin(), stale.end());
        stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
        for (auto id : stale)
            g_param_ptrs.erase(id);
        spdlog::warn("[COLLECTED] Evicted {} stale param pointer(s) (write AV); {} remain",
                     stale.size(), g_param_ptrs.size());
    }
    if (missed > 0)
        spdlog::warn("[COLLECTED] {} row IDs NOT in param_ptrs (out of {} collected)", missed, new_collected.size());

    int delta = (int)new_collected.size() - (int)g_collected_rows.size();
    g_collected_rows = std::move(new_collected);
    g_collected_count = (int)g_collected_rows.size();

    spdlog::info("[COLLECTED] Refresh: {} hidden (delta {:+d}), {} GEOF entries, {} WGM tiles",
                 g_collected_count, delta, geof.size(), wgm_tiles.size());

    return delta;
}

// ─── queries ────────────────────────────────────────────────────────

bool goblin::collected::is_row_collected(uint64_t row_id)
{
    return g_collected_rows.count(row_id) > 0;
}

bool goblin::collected::is_original_row_collected(uint64_t original_row_id)
{
    auto it = g_original_to_dynamic.find(original_row_id);
    uint64_t id = (it != g_original_to_dynamic.end()) ? it->second : original_row_id;
    return g_collected_rows.count(id) > 0;
}

int goblin::collected::collected_count()
{
    return g_collected_count;
}

int goblin::collected::skipped_count()
{
    return g_unmatched_count;
}
