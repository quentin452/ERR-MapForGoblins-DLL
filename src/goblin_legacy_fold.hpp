#pragma once
#include <cstdint>
#include <cstddef>
#include "goblin_dll_export.hpp"

// ─── Live legacy-dungeon -> overworld fold ───────────────────────────────────
//
// Replaces the baked LEGACY_CONV table with a fold computed from the LIVE
// regulation param `WorldMapLegacyConvParam`. Equivalent to the engine's runtime
// fold FUN_1408775e0, but map-CLOSED (the param is resident with the regulation,
// no WorldMapViewModel needed) and with the corrections the static baker can't
// make: full-block key (area,gx,gz), terminal = area in [50,88] (FUN_140660fe0),
// and chain composition at fold time (m35 -> 11 -> 60). See
// docs/re/windows_legacyconv_param_live_re_findings.md. Live source = no
// per-mod drift; callers fall back to the baked table when unavailable.
namespace goblin::legacy_fold {

struct Folded {
    uint8_t area, gx, gz;   // folded block (overworld/field frame)
    float posX, posZ;       // block-local coords
    float ent_x, ent_z;     // terminal dst base-point world (the overworld entrance)
    bool matched;           // a conv chain applied (else leave the row as-is)
};

// Build the lookup from the live param if not already built. No-op (returns
// false) when params aren't loaded yet. Cheap once built.
bool ensure_built();

// Drop the cached table; rebuilt lazily on the next fold (call on regulation reload).
void invalidate();

// True when the live param produced a usable table. False -> use baked LEGACY_CONV.
bool available();

// Identity/terminal test (FUN_140660fe0): area in [50,88] is overworld/field,
// not a fold source.
inline bool is_terminal(uint8_t area) { return (uint8_t)(area - 0x32) < 0x27; }

// Fold a block-local marker down the conv chain. matched=false when no row applies.
Folded fold(uint8_t area, uint8_t gx, uint8_t gz, float posX, float posZ);

} // namespace goblin::legacy_fold

// ─── Fallback entrance anchor (Slice 1 of dungeon_entrance_fallback_anchor_plan) ─────────────
// When legacy_fold DECLINES an area (params resident but no WorldMapLegacyConvParam row for the
// block — a mod-added dungeon like ERR area 45), project_dungeon_row_to_overworld consults this
// per-source-area table: an overworld ENTRANCE (a ConvRow-shaped dst) to collapse that dungeon's
// markers onto, instead of leaving them at the block-local origin pile. Empty = no-op (today's
// behaviour). Seeded later by Slice 2/3 (eventFlagId correlation / EMEVD warp) or manually via the
// `entrance_anchor` debug RPC. Mirrors legacy_fold's dst (entrance world = dst_gx*256+dst_px, …).
namespace goblin::entrance_anchor {

struct Anchor { uint8_t dst_area = 0, dst_gx = 0, dst_gz = 0; float dst_px = 0.f, dst_pz = 0.f; };

GOBLIN_RENDER_API void set(uint8_t src_area, Anchor a);  // register/replace the fallback for src_area
GOBLIN_RENDER_API void reset();                          // clear all
GOBLIN_RENDER_API const Anchor *get(uint8_t src_area);   // nullptr when no fallback for this area
GOBLIN_RENDER_API size_t count();

} // namespace goblin::entrance_anchor
