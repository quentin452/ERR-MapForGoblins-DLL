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
    return best;
}

} // namespace

void invalidate() {
    g_built = false;
    g_available = false;
    g_by_block.clear();
    g_by_area.clear();
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
            // First base point per full block wins (engine tree key is the packed block).
            g_by_block.emplace(block_key(c.src_area, c.src_gx, c.src_gz), c);
            g_by_area[c.src_area].push_back(c);
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
        if (!r)
            break;
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
