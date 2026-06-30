---
name: one-dll-externalize-mapdata
description: "Plan to kill the N-DLL-per-mod problem. SUPERSEDED 2026-06-30: no per-mod data file (externalized or baked) will exist once vanilla/ERTE/Convergence migrate to live DiskMSB like ERR — so the runtime-pick decision (A/B/C/D below) is dead, not just parked. Next step is the migration itself, see docs/HANDOFF.md."
metadata: 
  node_type: memory
  type: project
---

**UPDATE 2026-06-27 — RE-FRAMED by no-bake.** The premise (a 3.37MB compile-time bake to
externalize) is gone for ERR: no-bake Phase 2 shipped (older note: HEAD `4a7716d`; import checkout
2026-06-28: `master@931438d`, baked=0, 61/61 off-bake;
see [[msbe-parser-supersedes-bake]]). ERR now reads markers live from the mod's own
`map/MapStudio/*.msb.dcx` (DiskMSB parser) + live game memory → it ALREADY works per-mod with
ONE build, no baked array. The real path to one-DLL-for-all is therefore NOT "externalize the
bake" but "migrate vanilla/ERTE/Convergence onto the same DiskMSB+live pipeline" (their
`generated_*/goblin_map_data.cpp` still ship full `MAP_ENTRIES`). The variant-select problem
shrinks accordingly. Original parked design below kept for context.

**Problem (user, 2026-06-23):** the old maintainer had to ship a SEPARATE DLL per mod —
`Convergence/ERR/ERTE/Vanilla - MapForGoblins - DLL - v2.0.0.zip` (4 zips). Want to avoid that.

**Root cause (verified):** the baked map data is a **compile-time C++ array** —
`src/generated/goblin_map_data.cpp` (3.37 MB, `MAP_ENTRIES[]` = 8653 rows) — selected per-mod by a
CMake switch (`-DGENERATED_SUBDIR=generated_vanilla`, defines `MFG_VANILLA`/`MFG_PROFILE_VANILLA`; a
parallel `generated_vanilla/` dir already exists = 2.88 MB). Each mod's regulation+MSB → different
placements/iconIds/categories → different `.cpp` → **separate compile → separate DLL.** Positions are
irreducibly per-mod AND must stay baked (no runtime source — proven, see [[loot-identity-stable-err-additive]]),
so the fix is NOT "go live", it's **stop compiling the data into the binary.** Related: [[live-param-vs-baked-data]].

**Why:** one binary to build/sign/maintain; data updates need no rebuild; a new mod = run the
generator only, zero compile. Kills the N-DLL problem permanently.

**Plan (one DLL + per-mod data files):**
1. `tools/generate_data.py` emits `mapdata_<variant>.bin` (packed fixed-record array + deduped string
   pool + header{count, format-version}) instead of the `.cpp`.
2. New loader (`goblin_map_data_loader.cpp`): at startup load the `.bin` into a `std::vector<MapEntry>`
   + string arena; expose the SAME `MAP_ENTRIES`/`MAP_ENTRY_COUNT` accessors so the ~13 consumers
   (goblin_inject/collected/markers/messages/kindling/overlay, worldmap/map_entry_layer, …) barely change.
3. Same treatment (or keep compiled if shared) for the other per-variant tables: `enemy_names`
   (370 KB vanilla), `item_icons` (89 KB). `generated_shared/` stays compiled.

**Serialization is easy:** `MapEntry` = all POD (`row_id` u64, fixed `WORLD_MAP_POINT_PARAM_ST` param
struct, `category` u8, `geom_slot`/`name_suffix` i16, `lotId` u32, `lotType` u8) EXCEPT one string
`object_name` → record array + offset into a string pool. ~8653 records ≈ few hundred KB, same-platform
endianness.

**DEAD DECISION — how the single DLL picks which .bin at runtime (A/B/C/D below), confirmed dead 2026-06-30:**
- **A: auto by `regulation.bin` hash.** **B: INI key `map_data_file=`.** **C: auto-detect by mod folder/files.**
  **D: bundle all `.bin` + try-match.** (kept for the record only — see below for why none apply)

**Why dead, not parked:** all four options assume a per-mod data FILE that the runtime must pick between.
Once vanilla/ERTE/Convergence migrate onto the same live-DiskMSB pipeline ERR already uses (no-bake
Phase 2, see [[msbe-parser-supersedes-bake]]), there is no per-mod file left to pick — one binary reads
whatever mod's `map/MapStudio/*.msb.dcx` + regulation is on disk at runtime, same code path for every
mod. "Pick a data file" stops being a question because there's no data file. User confirmed 2026-06-30:
"pas besoin de runtime detection, un dll est suffisant pour tout" (no runtime detection needed, one DLL
suffices for everything). Next step is the migration itself (vanilla first, then ERTE, then Convergence),
tracked in `docs/HANDOFF.md`.
