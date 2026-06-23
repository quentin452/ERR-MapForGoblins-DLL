# Findings — live loot/treasure WORLD position

Companion to `windows_live_loot_position_re_prompt.md`.

---

## §0 pre-check — do baked loot positions drift vs the current ERR MSB? → **NO (verdict: SKIP the RE)**

**One-line:** A virgin re-extract from the current ERR MSB reproduces every committed baked loot
position **exactly** (0 of 30,305 shared placements moved). Baked positions are faithful; the
runtime live-position RE buys design-purity only, not correctness — **do not start it now.**

**Method (offline, no RE):** re-ran `tools/extract_all_items.py` against the on-disk ERR mod
(`D:\DOWNLOAD\ERR_mod`, ERR 2.2.9.6 — the same version the committed bake came from), then diffed
positions against the committed `data/items_database.json` (pulled from git HEAD). Join key =
`(map, partName, itemLotId, source)` over records carrying a real MSB `partName` (a physical
placement); compared `(x,y,z,areaNo,gridX,gridZ)`.

| metric | value |
|---|---|
| records total (baseline = fresh) | 31089 |
| placed records (have partName) | baseline 30341 / fresh 30305 |
| shared placement keys | 30305 |
| **positions moved** | **0** |
| only in baseline | 36 (EMEVD enrich-fallback rows added *after* extraction — virgin extract is pre-enrich; not moved chests) |
| only in fresh | 0 |

`npc_name_ids.json` and `unreachable_msb_lots.json` also re-extracted byte-for-byte.

**Caveat — what this does and does NOT prove:** only ERR 2.2.9.6 is on disk (= the bake's
version), so this confirms the extraction is **faithful and deterministic** for the current
version. It cannot prove a *future* ERR version won't relocate chests. But the §0 reasoning holds:
ERR is additive and the randomizer shuffles lot *contents*, never the chest/enemy placement — so
position drift is not expected even across versions.

**Decision:** position stays **baked**; the live-position RE (treasure/asset placement system,
`WORLD_GEOM_MAN_SLOT` walk, enemy-drop ChrIns) is **not justified** — it's HIGH effort for a drift
that measures zero. The cheap, real future-proof is to **re-run this diff on each ERR version
bump** (a position-coverage check), not a runtime read. Revisit the RE only if a future bump shows
many placements moved, or if full runtime-purity (zero baked loot data) becomes an explicit goal.

The reproducible diff lives in this session's command log (git-HEAD baseline vs `extract_all_items`
re-run); fold it into a `tools/diag_loot_position_drift.py` if it needs to run per version bump.

---

## T1 + T3 — the asset placement system is ALREADY RE'd (we walk it for collected-state)

Launched the RE anyway (per request). Outcome: **the hard part is already done.** `goblin_collected.cpp`
already enumerates `CSWorldGeomMan` and reads, per loaded asset instance, its **runtime world
position + its MSB part name** — which is exactly T1's target *and* T3's join key. No new manager RE
is needed; only wiring.

**The chain (live-confirmed in goblin_collected.cpp, RVAs re-anchored via AOB):**

| step | offset | meaning | source |
|---|---|---|---|
| `CSWorldGeomMan` static | `sig::WORLD_GEOM_MAN_SLOT` (`re_signatures.hpp`) | manager | already owned |
| BlockData rb-tree node | `+0x20` = MapId key, `+0x28` = BlockData* | per-tile | [collected:381](../../src/goblin_collected.cpp) |
| geom_ins vector | `BlockData+0x288` (begin `+0x08`, end `+0x10`) | loaded asset instances | [collected:388](../../src/goblin_collected.cpp) |
| MsbPart pointer | `geom_ins+0x48` (`0x18*3`) | the placed part | [collected:439](../../src/goblin_collected.cpp) |
| **name** | `MsbPart+0x00` → wchar* | `"AEG099_990_9002"` — **the T3 join key** | [collected:447](../../src/goblin_collected.cpp) |
| **world pos** | `MsbPart+0x20` (3×f32 X/Y/Z) | **the live loot position** | [collected:471](../../src/goblin_collected.cpp) |
| alive / collected | `geom_ins+0x263` bit1 / `+0x26B` bit4 | (already used for collected) | [collected:488](../../src/goblin_collected.cpp) |

**T1 — verdict: SOLVED.** Treasure chests are `AEG099_990_*` assets — already inside the
`AEG099_*` family `goblin_collected` keeps (it ignores AEG001 decorations/colliders). The walk
already yields `(MapId, partName, x, y, z)` per chest. The runtime pos is MSB-part-local; reuse the
block→world / map-UI transforms we own (`FUN_1408775e0` / `FUN_140876140`) exactly as the player/boss
paths do.

**T3 — verdict: SOLVED (join by partName).** A live instance exposes its MSB part name; the baked
`items_database.json` already keys every placement by `(map, partName)` → `itemLotId`. So
live `partName` → baked record → `itemLotId` → marker. No need to read an ItemLotID off the live
asset (the geom instance is geometry; it carries the name, not the lot).

**T2 (enemy drops, lotType==2) — pos is the enemy ChrIns, NOT a geom asset.** Those need the
WorldChrMan enemy enumeration that was abandoned in
`windows_enemy_boss_runtime_pos_re_findings.md`. So a geom-based live-position path covers
**treasure/map lots (lotType==1) only**; enemy-drop markers stay baked.

**T4 / limitation:** only assets in currently-loaded tiles have a live instance — the geom vector
is per-loaded-BlockData. Loot in unstreamed regions has no live pos → must keep the baked fallback.

### Wiring needed (if we ever do it — not justified by §0)
1. Bake a `partName → (rowId/marker)` map (or `partName → lotId`) into the DLL so a live geom
   instance can be matched to its marker. (`items_database` already has `partName`; add it to the
   linkage sidecar.)
2. In the geom walk (reuse goblin_collected's already-running enumeration — don't add a second
   pass), for tracked `AEG099_990_*` chest instances, project `MsbPart+0x20` via the owned
   transforms and overwrite the marker's baked position for loaded tiles; fall back to baked when
   unloaded.
3. Treasure/map lots only; leave enemy-drop and unloaded markers baked.

### Pending — live confirmation (T2 sample)
`eldenring.exe` was NOT running this session (`config.find_eldenring_pid()` → None), so no live
RPM sample was taken. The static chain is fully validated by the shipping collected-state reader,
but a one-shot live read (pick a loaded chest, compare `MsbPart+0x20` projected vs its baked marker
pos) should confirm **live == baked** before any wiring — which would re-confirm §0 from the live
side and, per §0, remove the last reason to wire it.

**Bottom line:** the RE is essentially complete and shows the feature is *cheap to build* (reuse the
geom walk), but §0 already proved it's *unnecessary* (zero drift). Feasible ≠ justified — it stays
deferred unless a future ERR version relocates chests.
