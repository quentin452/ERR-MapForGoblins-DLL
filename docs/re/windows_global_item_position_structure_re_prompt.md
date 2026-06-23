# RE prompt — hunt for a GLOBAL resident item/asset position structure

> **Goal: confirm or REFUTE that a global, always-resident structure holds the world positions of
> ALL placed map items/treasures (not just the ones loaded near the player).** If it exists, it
> unblocks a full switch from baked loot positions to live ones; if it does not, it definitively
> closes the question and proves baked position is mandatory. App = current ERR build (Elden Ring
> 1.x + ERR 2.2.9.6); re-anchor every RVA/AOB yourself.

---

## 0. Why this prompt exists (read first — the exact gap)

We already read map-icon positions LIVE from GLOBAL resident params: bosses from `WorldMapPointParam`
(textId2==5100), graces from `BonfireWarpParam`. Those are resident param tables → every row present
regardless of where the player stands. **Loot/treasure positions have NO such param** — the position
lives in the per-tile MSB (`Events.Treasure` links a `Parts.Asset` chest to an `ItemLotID`), so today
we BAKE it from offline MSB extraction.

We validated the runtime read 3 ways (offline diff 0/30305 drift + external RPM + an in-process DLL
probe `[LOOTPOS]`, all byte-exact). **But the runtime asset read is via `CSWorldGeomMan`, which only
contains LOADED tile instances** — a moving bubble around the player. At Limgrave the probe compared
only ~20 assets (the loaded ones). So a live read CANNOT cover the whole map: far/unloaded treasures
have no `CSWorldGeomMan` instance → no position. That is the blocker this RE attacks.

**User hypothesis (the thing to test):** "logically the engine must track every item's position
somewhere global — behind/above the loaded-instance structure there is probably a resident table of
all positions." Plausible (the streamer must know where things are to schedule loads) — but ER is an
open-world STREAMING engine, so it is equally plausible that placement data lives ONLY in the
on-disk, per-tile MSB (decompressed when the tile streams in) and there is NO resident global
position table. **Your job is to settle which is true, with evidence — do not assume it exists.**

---

## 1. The decisive experiment FIRST (cheap, binary verdict — do this before any deep RE)

This one Cheat-Engine test answers the whole question and tells you whether the rest of the prompt is
even worth running:

1. Pick a treasure with a KNOWN baked world position in a tile FAR from the start (use our
   `data/items_database.json` — pick a map lot, e.g. a chest in Caelid/Altus, note its MSB-local
   `posX/posY/posZ` and its tile `m60_xx_yy`; world = `gridX*256 + posX`, same convention as
   re_findings_playerpos.md).
2. Start a NEW-game or stand somewhere FAR so that tile is NOT loaded.
3. In Cheat Engine, do a FLOAT search for that asset's position component(s) (try MSB-local first,
   then world-absolute). Search X, then "same-value" filter on Y and Z near it (a 3-float run at
   +0/+4/+8 or +0x20 layout).
   - **If you find a resident 3-float run matching the unloaded chest's known position → a GLOBAL
     structure EXISTS.** Trace it: "what accesses this address" + pointer-scan to a static base; then
     §2/§3 to characterize it.
   - **If the position is NOWHERE in memory until you travel to that tile (then it appears in
     `CSWorldGeomMan`) → NO global position table.** Refute the hypothesis, write that verdict, and
     stop. Baked position is then proven mandatory (the engine streams placement from disk per tile).
4. Sanity-pair it: confirm the SAME chest's position DOES appear once you load its tile (so the search
   method is valid — you're not missing it due to a wrong value/encoding).

Report this result explicitly either way — a clean REFUTE is a valuable, shipping-relevant outcome
(it closes [[loot-identity-stable-err-additive]] permanently).

---

## 2. If it exists — candidate owners to identify (where a global table would live)

Hunt the managers/registries that could hold all placements resident. Use the cleartext RTTI in
`eldenring.exe` (FD4 class names are present) + the FD4Singleton finder pattern.

- **`CSWorldGeomMan` parent / siblings.** We already resolve `WORLD_GEOM_MAN_SLOT` (src/re_signatures.hpp;
  walk = manager → BlockData rb-tree @+0x18 → geom_ins vec @BlockData+0x288 → MsbPart @+0x48,
  name@+0x00, pos@+0x20; see windows_live_loot_position_re_findings.md + goblin_collected.cpp). The
  rb-tree holds only LOADED BlockData. Check: is there a sibling/parent collection that enumerates
  ALL blocks (loaded + not), and do unloaded BlockData retain placement metadata or just AABB/stream
  state?
- **The open-world streaming / tile-grid manager** (the thing that decides what to load): search RTTI
  for `*WorldArea*`, `*OpenField*`, `*MapTile*`, `*Grid*`, `*Stream*`, `CSFD4World*`. It must know
  tile bounds; check whether it also indexes per-asset placements (for load budgeting) or only AABBs.
- **A resident MSB / map-data cache.** Does ER keep a lightweight global index of `Parts.Asset` /
  `Events.Treasure` entries (e.g. for fast-travel, the map menu, or asset-budget) separate from the
  streamed full MSB? Search `*MsbRes*`, `*MapData*`, `*AssetMan*`, `*Treasure*`, `*ItemMan*`.
- **The item-pickup / save system.** ER tracks collected items by event flag (we use this:
  windows_collected_loot_flag_re_findings.md + windows_geom_flag_savedata_table_re_findings.md). That
  flag/geof table is GLOBAL and resident — check whether any entry there carries a WORLD POSITION
  (probably not; it's flag-state only) or links to one. The geof save table (manager+0x189d0 count,
  entries @+0x10) is the most promising resident global already in our codebase — inspect an entry's
  full layout for any position floats.
- **`WorldMapPointParam` item rows.** We use 5100=boss. Enumerate the other `textId2`/icon types — is
  there any map-point row family that encodes ITEM/treasure positions globally the way it does bosses?
  (Likely not for generic loot, but cheap to rule out via the param we already read.)

---

## 3. If it exists — deliverable layout

For the structure you find, document (so we can wire it into the DLL exactly like bosses/graces):
- **Static base**: AOB or `er_base + RVA` to the manager (add to the registry style of
  src/re_signatures.hpp). Prefer an AOB on a stable code ref.
- **Enumeration**: how to walk EVERY entry (container type — vector/rb-tree/hashmap — count field,
  stride, begin/end). Must cover unloaded placements, that's the whole point.
- **Per-entry layout**: world position (X/Y/Z offsets + whether MSB-local or world-absolute), MapId /
  tile (for the block→world transform we already own, `FUN_1408775e0`/`FUN_140876140`), and a JOIN key
  to a lot — `ItemLotID` or the MSB entity id we can map to a lot via `items_database.json`
  (map,partName)→lotId. Without a lot join the positions are unusable.
- **Residency/lifetime**: confirm it survives tile load/unload + fast travel (stand far, re-read —
  values stable), and that it's populated at map load (not lazily on first visit).
- **Cost**: entry count (all map items ≈ tens of thousands) + read cost — flag any Wine RPM-walk cliff
  ([[linux-rpm-walk-danger]]: bulk-read whole entries, 1 RPM/entry not N).

---

## 4. Method notes

- Reuse, don't rebuild: the loaded-asset walk, the block→world transforms, the geof save table, and
  the boss/grace live-param reads are all already RE'd and running — see the findings docs named above
  and `src/goblin_collected.cpp` / `src/goblin_inject.cpp`.
- Cheat Engine "find what accesses" on a resident position, then pointer-scan to a static base; confirm
  the chain across a game restart (a raw heap address won't survive — needs a base+offset chain).
- Crash-safety on the DLL side later: `ReadProcessMemory` / `rpm<T>`, never `__try`
  ([[clang-cl-seh-noinline]]).
- Deliver `docs/re/windows_global_item_position_structure_re_findings.md`: EXISTS (with §3 layout) or
  DOES-NOT-EXIST (with the §1 evidence). Either verdict ships.

---

## 5. Honesty / scope check (don't oversell)

Even if a global table exists, remember the original verdict: loot positions do **not drift** (ERR is
additive; randomizers shuffle lot *contents*, never move the chest — proven 0/30305). So a global live
read buys **design-purity (zero baked loot data) + automatic immunity to a future relocating ERR
bump**, NOT a current bug fix. The realistic alternative remains: keep baked, re-diff per ERR version
bump. Weigh fleet time accordingly — but the §1 refute/confirm is cheap and worth doing regardless,
because it permanently settles whether the switch is even possible.
