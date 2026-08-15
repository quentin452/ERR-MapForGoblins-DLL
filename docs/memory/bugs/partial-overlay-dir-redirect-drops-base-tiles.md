# Partial-overlay mod: the map-dir redirect dropped the base catalog (invisible Gatefront chest)

**Status: RESOLVED 2026-08-15, fix shipped + deployed to the GA install (needs a game restart).**
Live-verified pre-fix via RPC; post-fix verification pending (freshness: `mfg_build` + game restart).

## Symptom

On Golden Age (partial-overlay mod: ships 226/1347 tiles in `<root>/GA\map\MapStudio`, the rest
come from the base install), a chest standing next to the player at Gatefront (tile m60_45_39)
had NO marker. The same chest WAS detected on ERR (`mod\map\MapStudio` covers the whole world,
651 tiles — that tile alone had 14 treasures / 14 positioned in the ERR log).

## Root cause

Two-phase map-dir resolution on a GA-like mount:

1. **Boot:** the ancestor-walk resolves the BASE install's MapStudio (`E:\SteamLibrary\...\map\
   MapStudio` — the walk only probes `<p>/mod/`, misses `<root>/GA/`). Full base catalog parsed:
   949 `_00` tiles, 3268 treasures. The player's tile m60_45_39 (vanilla, not overridden by GA)
   IS covered at this point.
2. **Redirect:** the first streamed GA MSB makes `on_map_opened_path` (loot_disk.cpp) flip
   `g_resolved_dir` to `GA\map\MapStudio` + `force_disk_rebuild()`. The rebuild re-parses ONLY
   the mod dir → 154 `_00` tiles, 1793 treasures → **every non-override base tile drops out of
   the parse** (the player's tile → 0 enemies live via `vmap ename 60 45 39`; GA tiles show
   54-104 matches).

The RESIDENTMSB merge can't save this: it only covers tiles the game has STREAMED (captured via
CreateFileW), and the base tiles stream from the base install (their opens aren't mod data).

Why ERR never showed it: the walk lands directly on the COMPLETE `mod\map\MapStudio`; the game's
opens are from the same dir → the base-dir filter (`dir == g_walk_dir`) keeps it → no redirect →
no re-parse → the whole world stays covered.

## Fix

The per-tile disk readers now scan **mod dir + base dir merged**:

- `disk_loot_dirs()` (loot_disk.cpp): `{g_resolved_dir}` + `{g_walk_dir}` when they differ —
  the resolved (mod) dir first, the walk-found (base) dir second. One entry when identical
  (ERR/vanilla → behavior unchanged).
- `msb_tile_files()`: deduped enumeration across those dirs, case-insensitive by tile stem —
  **the mod's copy of a tile wins**, the base fills every tile the mod doesn't ship.
- Applied to `load_disk_treasures`, `load_lod_award_entities`, `load_lod_feature_assets`,
  `load_lod_treasures`, and the merchant talk-ESD packed candidates.
- `ParsedDisk` (map_entry_layer.cpp) cache key = the full dir list joined with `|`, so the
  redirect re-parses the merged catalog exactly once (re-key on either dir changing).

## Verification recipe (GA, after restart)

1. `mfg.py rpc mfg_build` — freshness (a stale DLL answers `ping` too).
2. `vmap ename 60 45 39` → must show the Gatefront enemies (was 0).
3. `[LOOTDISK]` log: `reading MSBs from GA\map\MapStudio + base E:\SteamLibrary\...` + tile
   counts ≈ 154 (GA) + ~1100 (base) instead of 154-only.
4. Gatefront chest marker visible on the vmap.

## Related

- Same family as the WALK-WINS-OVER-CAPTURE item in HANDOFF §B — but a distinct bug: that one is
  "walk-found dir beats the capture"; this one is "redirect REPLACES the base with the mod's
  partial overlay".
- The base-dir filter in `on_map_opened_path` (2026-08-15, rebuild-storm fix) is what makes the
  merge safe: opens from the base dir are ignored (no dir toggle), so `disk_loot_dirs()` stays
  stable at {mod, base}.
