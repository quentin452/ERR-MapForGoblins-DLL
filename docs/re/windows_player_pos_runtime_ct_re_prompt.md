# RE prompt — LIVE player position (overworld + UNDERGROUND), runtime-validated, with a Cheat Engine CT deliverable

**Status: BLOCKED on RUNTIME RE.** Static decompile alone has produced wrong offsets twice
(see "What we already tried"). We need a source for the player's world/map position that is
**verified live in Cheat Engine** on app **2.6.2.0 / ERR 2.2.9.6**, then handed back as (a) a
resolution recipe (AOB + deref chain + offsets) and (b) a **working .CT table** we can re-test.

We (the mod authors) build the DLL on **Linux (clang-cl + xwin)** and **cannot run Cheat
Engine / the game**. You (Windows agent) have CE + Ghidra + the running game. So the
deliverable must be runtime-proven, not just decompiler-derived.

## What we need (two consumers)

1. **Distance-adaptive clustering (map OPEN):** the player position in the overlay's UNIFIED
   marker space — the frame markers land in after
   `goblin::marker_world_pos(area, gridX, gridZ, posX, posZ, conv_underground=true)`
   (`src/goblin_inject.cpp`). Valid on EVERY page: overworld (m60/m61), base underground
   **m12** (Siofra/Ainsel/Nokron/Deeproot), DLC underground **m40–m43**.
2. **In-game MINIMAP (map CLOSED):** the SAME position, but read **while the world map is NOT
   open** (a HUD during normal gameplay). So the source must update live during play, not only
   when the map dialog exists.

Ground truth = the native **yellow "you are here" dot** the game draws on the world map. It is
correct on every page (incl. underground). Find the memory the game reads to draw it.

## What already works

`goblin::get_player_map_pos` (`src/goblin_inject.cpp`) returns area + world X/Z + tile and is
**correct OVERWORLD** via the geom/map-pos manager `+0x70`(X)/`+0x74`(Z) bridged with the MapId
tile. It is **wrong underground** (see below). The overworld path proves the unified-frame
conversion is right; only the underground *source* is missing.

## What we already tried — and the exact runtime failures (please don't re-derive these)

All offsets below are app 2.6.2.0 / ERR 2.2.9.6, imagebase 0x140000000. AOBs live in
`src/re_signatures.hpp`; all named ones resolve PASS unless noted.

1. **Geom / map-pos manager (CSWorldGeomMan), slot ≈ `er_base + 0x3d69ba8`** (AOB
   `WORLD_GEOM_MAN_SLOT`, shared with goblin_collected). Read `+0x70..+0x88`.
   - Overworld (tile 46,38): `+70=-7.7 +74=3.3 +78=0.5 +80=-11.2 +84=4.8 +88=0.6` → with
     `world = tile*256 + local` this lands ≈ on the yellow dot. **Works.**
   - Underground, **Ancestral Woods (Siofra), tile (2,0)**: `+70=1.5 +74=-5.3 +78=5.4
     +80=3.0 +84=-3.5 +88=3.7` → **all ≈ ORIGIN**, NOT the real location. Every offset
     +0x70..+0x88 reads garbage/near-zero underground. So this manager does NOT hold the
     underground player position at these offsets. (RE note had claimed +0x78 = the 3D Z; on
     this build it's also origin underground.)
2. **WorldChrMan physics chain** `*(WorldChrMan + 0x1e508)` (LocalPlayer) `→ +0x58 → +0x10 →
   +0x190 → +0x68` = a Vec(X,Y,Z). All hops deref **non-null** (chain resolves), but the Vec
   reads **(0.0, 0.0, 0.0)** at Ancestral Woods — AND read (0,0) on the OVERWORLD too. So the
   leaf offset (`+0x68`) or the `+0x190` node is **wrong for this build** (the prior findings
   `windows_underground_player_pos_re_findings.md` over-trusted the static decompile here).
   The older `+0x10EF8` LocalPlayer offset is dead (drifted to `+0x1e508`).
3. **Map-point manager builder `FUN_1406d3a20`** (RE doc claimed a distinct `DAT_143d69ba8`
   at `+0x70/+0x78`). Our AOB `MAPPOINT_MGR_BUILDER` (`48 89 5C 24 18 57 48 81 EC A0 00 00 00
   0F 29 B4 24 90 00 00 00 48 8B 05 ?? ?? ?? ??`, rel {{24,28}}) is **AMBIGUOUS (2 matches)**
   and resolves to the **same slot** as `WORLD_GEOM_MAN_SLOT` (both `er_base + 0x3d69ba8`).
   So "DAT_143d69ba8" is NOT a separate manager from CSWorldGeomMan — it's the same one that
   reads origin underground.

**Conclusion:** the underground player position is NOT in that manager at +0x70..+0x88, and
the physics chain's leaf is wrong. The real source is elsewhere (a different node off the
player, a sub-area/world-block transform, or the worldmap-dialog player-marker the yellow dot
is drawn from). This is what we need you to find at runtime.

## Deliverable

1. **A Cheat Engine `.CT` table** (commit it under `docs/re/` or attach) that, with no manual
   pointer-scan by us, exposes the player's LIVE position as float X/Z (and the area/tile if a
   bridge is needed), updating in real time, **verified on every page**:
   - overworld, base underground **m12** (test at ≥2 distinct spots — e.g. Ancestral Woods /
     Siofra and a Nokron or Ainsel grace), and **DLC underground m40–m43** if reachable.
   - The CT entry(ies) must be **AOB/pointer-anchored** (not a one-session hardcoded address),
     so they survive a restart.
2. **The resolution recipe**, precise enough to port into `src/re_signatures.hpp` +
   `goblin_inject.cpp`:
   - The anchoring **AOB** (unique — note match count) and how to get the static slot / base
     (any `relative_offsets`).
   - The full **deref chain + final X/Y/Z float offsets**.
   - Whether the value is **block-local** (needs `world = tile*256 + local` with the MapId tile)
     or already **unified map/marker space** (use directly). If block-local, confirm the tile
     source that is correct underground (MapId singleton `+0x2c`: area `>>24`, gridX `>>16`,
     gridZ `>>8`).
   - Confirm it updates **while the world map is CLOSED** (needed for the minimap), and that the
     SAME source also works OVERWORLD (so we can collapse to one code path).
3. **A short note** on the yellow-dot draw path you used as ground truth (the fn / struct the
   game reads), in case we later want to read it directly.

## How to validate (decisive runtime check)

Stand at a KNOWN underground grace; the value must equal that location and **move with the
player**. Cross-check against the native yellow dot on the open map. A correct source is
**non-origin underground** and tracks movement; the current manager/chain are origin/zero.

## Constraints / references

- Patch-resilient AOBs only in shipping code (RVAs allowed in the CT for convenience, but give
  us an AOB to re-derive the slot). Registry + health check: `src/re_signatures.hpp`
  (`all_signatures()` logs `[SIG] PASS/FAIL/MULTI`).
- Unified-frame conversion to reuse: `goblin::marker_world_pos` /
  `project_dungeon_row_to_overworld(conv_underground=true)` and `get_player_map_pos`
  (`src/goblin_inject.cpp`). Overworld output is the reference for "correct".
- Prior (partially-wrong) findings: `windows_underground_player_pos_re_findings.md`,
  `windows_underground_player_pos_re_prompt.md`, `re_findings_playerpos.md`.
- Note for reads: the mod reads game memory via **ReadProcessMemory** (clang-cl elides `__try`);
  not relevant to your CT, but it's why we need exact offsets (a wrong deref returns false, not
  a crash, so silent-wrong is the failure mode we're fighting).
