#pragma once
#include <cstdint>

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
