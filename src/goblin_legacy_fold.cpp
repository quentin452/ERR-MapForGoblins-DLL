#include "goblin_legacy_fold.hpp"

#include <cmath>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_LEGACY_CONV_PARAM_ST.hpp"

namespace goblin::legacy_fold {

namespace {

struct ConvRow {
    uint8_t src_area, src_gx, src_gz;
    float src_px, src_pz;
    uint8_t dst_area, dst_gx, dst_gz;
    float dst_px, dst_pz;
};

bool g_built = false;
bool g_available = false;
// Full-block key (area<<16 | gx<<8 | gz) -> row, plus a per-area list for the
// nearest-base-point fallback (markers whose grid matches no exact base block).
std::unordered_map<uint32_t, ConvRow> g_by_block;
std::unordered_map<uint8_t, std::vector<ConvRow>> g_by_area;
// Rows keyed by DST area — the reverse index. A "sub-map dead-end" area (Leyndell Ashen/Elden Throne
// 19/34/35: it is a DST of block (11,5,0) but a SRC of no row) has no forward fold, so its markers used
// to stay at raw grid ~0 = the bottom-left off-map corner. reverse_lookup lifts such a marker to its
// PARENT frame (the row's src), after which the normal forward loop folds the parent to a terminal.
std::unordered_map<uint8_t, std::vector<ConvRow>> g_by_dst_area;

inline uint32_t block_key(uint8_t a, uint8_t gx, uint8_t gz) {
    return ((uint32_t)a << 16) | ((uint32_t)gx << 8) | (uint32_t)gz;
}

// Exact full-block row, else the nearest same-area base point by grid distance
// (parity with the previous baked-table lookup; covers off-base markers).
const ConvRow *lookup(uint8_t area, uint8_t gx, uint8_t gz) {
    auto it = g_by_block.find(block_key(area, gx, gz));
    if (it != g_by_block.end())
        return &it->second;
    auto al = g_by_area.find(area);
    if (al == g_by_area.end())
        return nullptr;
    const ConvRow *best = nullptr;
    int best_dist = 0x7fffffff;
    for (const auto &c : al->second) {
        int dgx = (int)c.src_gx - (int)gx; if (dgx < 0) dgx = -dgx;
        int dgz = (int)c.src_gz - (int)gz; if (dgz < 0) dgz = -dgz;
        int d = dgx + dgz;
        if (d < best_dist) { best_dist = d; best = &c; }
    }
    // A same-area base point MANY blocks away is a DIFFERENT dungeon, not this one's off-base
    // marker (multi-tile dungeons span 1-2 blocks). Unbounded, ERR's Roundtable copy m31_90
    // matched the m31_22 catacomb 68 blocks away, snapped out-of-range, and dumped every
    // m31_90 item/merchant marker at that catacomb's entrance (user-caught 2026-07-07).
    // No match ⇒ the caller treats the map as unmappable (markers stay raw/off-map = hidden).
    if (best && best_dist > 6)
        return nullptr;
    return best;
}

// A row whose DST is `area` and whose SRC is a DIFFERENT area — used to lift a sub-map dead-end
// (no forward row) into its parent frame. Prefer a src that itself has a forward row (i.e. can be
// folded onward toward a terminal); else take any. nullptr if `area` is not a dst of any row.
const ConvRow *reverse_lookup(uint8_t area) {
    auto it = g_by_dst_area.find(area);
    if (it == g_by_dst_area.end())
        return nullptr;
    const ConvRow *fallback = nullptr;
    for (const auto &c : it->second) {
        if (c.src_area == area)
            continue;                       // not a real lift
        if (!fallback)
            fallback = &c;
        if (g_by_area.count(c.src_area) || is_terminal(c.src_area))
            return &c;                      // src can fold onward → best
    }
    return fallback;
}

} // namespace

void invalidate() {
    g_built = false;
    g_available = false;
    g_by_block.clear();
    g_by_area.clear();
    g_by_dst_area.clear();
}

bool ensure_built() {
    if (g_built)
        return g_available;
    g_built = true;
    size_t n = 0;
    try {
        for (auto [id, row] :
             from::params::get_param<from::paramdef::WORLD_MAP_LEGACY_CONV_PARAM_ST>(
                 L"WorldMapLegacyConvParam")) {
            ConvRow c{ row.srcAreaNo, row.srcGridXNo, row.srcGridZNo, row.srcPosX, row.srcPosZ,
                       row.dstAreaNo, row.dstGridXNo, row.dstGridZNo, row.dstPosX, row.dstPosZ };
            // One block can carry SEVERAL conv rows with different dst (e.g. Leyndell block (11,5,0) has
            // dst→60 overworld AND dst→19/34/35 sub-maps). First-wins by param row-id picked the WRONG one
            // when a dead-end (area 19 has 0 further rows) came first → the marker folded to grid~0 = (0,0)
            // off-map (57 Leyndell/Ashen graces+loot, `vmap offmap`). Fix: a row whose dst is a TERMINAL
            // (overworld, area∈[50,88]) beats a non-terminal (intermediate/dead-end) dst for the same block.
            {
                uint32_t bk = block_key(c.src_area, c.src_gx, c.src_gz);
                auto ins = g_by_block.emplace(bk, c);
                if (!ins.second && !is_terminal(ins.first->second.dst_area) && is_terminal(c.dst_area))
                    ins.first->second = c;   // upgrade to the direct-to-overworld row
            }
            g_by_area[c.src_area].push_back(c);
            g_by_dst_area[c.dst_area].push_back(c);
            ++n;
        }
    } catch (...) {
        // Param not loaded yet — leave g_built=false so we retry next call.
        g_built = false;
        return false;
    }
    g_available = n > 0;
    spdlog::info("[LEGACY-FOLD] live WorldMapLegacyConvParam: {} rows, {} blocks",
                 n, g_by_block.size());
    return g_available;
}

bool available() { return g_built && g_available; }

Folded fold(uint8_t area, uint8_t gx, uint8_t gz, float posX, float posZ) {
    Folded f{};
    f.area = area; f.gx = gx; f.gz = gz; f.posX = posX; f.posZ = posZ;
    f.matched = false;
    if (!ensure_built())
        return f;

    double wx = gx * 256.0 + posX, wz = gz * 256.0 + posZ;
    uint8_t a = area, cgx = gx, cgz = gz;
    bool any = false;
    for (int guard = 0; guard < 8; ++guard) {
        if (is_terminal(a))
            break;
        const ConvRow *r = lookup(a, cgx, cgz);
        if (!r) {
            // No forward row for `a`. If `a` is a sub-map dead-end (a DST of some row, e.g. Leyndell
            // Ashen/Elden Throne 19/34/35), lift it INTO its parent frame via that row's inverse, then
            // retry — the parent (area 11) does have a forward row to the overworld terminal (a60). This
            // is what fixes Elden Beast / Fractured Marika / all Ashen-Capital markers folding to (0,0).
            // ONLY for areas with ZERO forward rows (the true dead-ends): an area that HAS forward
            // rows but no row for THIS block (ERR's Roundtable copy m31_90 in the catacomb area 31)
            // must stay unmatched, not get lifted through an unrelated row — area 31 is a dst of
            // m14→m31_6 (row 135, the Academy waygate exit), so m31_90 was lifted into the Academy
            // frame, overflowed, and snapped every marker onto the Academy's map anchor (8848,11714).
            const ConvRow *rev = g_by_area.count(a) ? nullptr : reverse_lookup(a);
            if (rev) {
                wx += (rev->src_gx * 256.0 + rev->src_px) - (rev->dst_gx * 256.0 + rev->dst_px);
                wz += (rev->src_gz * 256.0 + rev->src_pz) - (rev->dst_gz * 256.0 + rev->dst_pz);
                a = rev->src_area;
                // Select the parent's EXACT block, not wx/256 — the nearest-base fallback in lookup()
                // ignores terminal preference and would return the parent's row BACK to this sub-map
                // (11->19), ping-ponging to a net-zero no-op. The exact block hits g_by_block, which the
                // terminal-upgrade prefers to route straight to the overworld (11->60).
                cgx = rev->src_gx;
                cgz = rev->src_gz;
                // Provisional entrance = the lifted position, so the out-of-range snap below never
                // targets (0,0) if (pathologically) no forward hop follows. A forward hop overwrites it.
                f.ent_x = (float)wx;
                f.ent_z = (float)wz;
                any = true;
                continue;   // retry from the parent frame (now foldable to a terminal)
            }
            break;
        }
        wx += (r->dst_gx * 256.0 + r->dst_px) - (r->src_gx * 256.0 + r->src_px);
        wz += (r->dst_gz * 256.0 + r->dst_pz) - (r->src_gz * 256.0 + r->src_pz);
        a = r->dst_area;
        // entrance = the latest hop's dst base; after the loop it's the terminal
        // overworld base point (cluster anchor for this dungeon's markers).
        f.ent_x = (float)(r->dst_gx * 256.0 + r->dst_px);
        f.ent_z = (float)(r->dst_gz * 256.0 + r->dst_pz);
        cgx = (uint8_t)(wx / 256.0);
        cgz = (uint8_t)(wz / 256.0);
        any = true;
    }
    f.matched = any;
    if (!any)
        return f;

    // Out-of-range guard (matches project_dungeon_row_to_overworld): abnormal
    // local coords can push wx/wz past the 0x3F-tile extent and wrap gridXNo
    // (uint8) -> the engine's icon build crashes. Snap to the entrance base point.
    if (wx < 0.0 || wz < 0.0 || wx > 0x3F * 256.0 || wz > 0x3F * 256.0) {
        wx = f.ent_x;
        wz = f.ent_z;
    }
    int fgx = (int)std::floor(wx / 256.0);
    int fgz = (int)std::floor(wz / 256.0);
    f.area = a;
    f.gx = (uint8_t)fgx;
    f.gz = (uint8_t)fgz;
    f.posX = (float)(wx - fgx * 256.0);
    f.posZ = (float)(wz - fgz * 256.0);
    return f;
}

} // namespace goblin::legacy_fold
