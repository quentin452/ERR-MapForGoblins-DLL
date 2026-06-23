# RE prompt — runtime enemy / boss world position (dungeon-boss DRIFT)

> **Goal:** decide, with hard data, whether reading a **live enemy/boss ChrIns world
> position** can fix the dungeon-boss marker DRIFT — and if so, expose a stable
> DLL-readable chain. App = current ERR build (base AOBs below were valid on ER 2.6.2.0;
> re-anchor every RVA, never hardcode).
>
> **Run on Windows** (Ghidra headless + Cheat Engine live), same toolchain as
> `re_findings_playerpos.md`. Deliver a `*_re_findings.md` next to this file.

---

## 0. The problem (what drifts and why)

Vanilla ER draws a **field-boss marker** (`WorldMapPointParam` row with `textId2 == 5100`)
on the overworld at the correct spot. Our overlay does NOT use that row's own map
position — instead `tools/generate_boss_list.py` cross-references the field-boss row to its
**MSB entity** and bakes the **entity's dungeon-internal position** into `data/boss_list.json`:

```json
{ "areaNo": 32, "gridX": 0, "gridZ": 0, "x": 83.314, "y": -24.549, "z": -25.44,
  "map": "m32_00_00_00", "enemyModel": "c3451", "npcParamId": 34510912,
  "vanillaPlaceName": "Scaly Misbegotten" }
```

For an **overworld** boss (m60/m61) the internal pos already IS overworld → fine.
For a **dungeon-internal** boss (m31 caves, m32 catacombs, m12 underground, …) the pos is in
the dungeon's own MSB frame. We then project it to the overworld via:

- live: `goblin_legacy_fold.cpp::fold()` (reads live `WorldMapLegacyConvParam`), then
  `goblin_worldmap_probe.cpp::project()` (engine `FUN_140876140` over the WorldMapViewModel
  converters);
- baked fallback: `src/generated/goblin_legacy_conv.hpp` `LEGACY_CONV[]` + nearest-base-point.

**Result:** dungeon-boss markers land far from the dungeon **entrance/grace** where players
(and MapGenie) expect them. `WorldMapLegacyConvParam` maps a dungeon's *grid block* to the
overworld, but the boss's internal `(posX,posZ)` inside that block does not correspond to the
entrance — so projecting the internal layout coord scatters it.

### Prior conclusion to VALIDATE or REFUTE (this is the crux)

The session handoff concluded runtime enemy pos **does NOT** fix the drift, because:
1. a loaded enemy reports the **same dungeon-internal coords** we already bake (so projecting
   it drifts identically);
2. **unloaded** dungeon enemies have no live pos at all (you must be inside the dungeon);
3. it needs a **large WorldChrMan enemy-enumeration RE**.

**Your #1 job: prove or disprove (1) with a live read.** If a loaded dungeon boss's runtime
world position, pushed through our exact projection, lands at the SAME wrong spot as the baked
value → runtime reading is a dead end and we go to a design fix (collapse-to-entrance). If it
lands somewhere BETTER (e.g. the engine resolves it to an overworld frame we're not using) →
runtime reading is worth wiring. **Do not assume — measure.**

---

## 1. What we already own (reuse, don't re-derive)

From `re_findings_playerpos.md` + `goblin_inject.cpp` (all live-confirmed):

- **WorldChrMan static:** `eldenring.exe + 0x3D65F88` (AOB `48 8B FA 0F 11 41 70 48 8B 05`,
  slot = finder + 0xE + disp32@+0xA). `sig::WCM_FINDER` in `src/re_signatures.hpp:66`.
- **LocalPlayer (the player ChrIns):** `[WorldChrMan + 0x1E508]`.
- **Player physics pos (block-local Vec):** `float [LocalPlayer + 0x6C0 / +0x6C4 / +0x6C8]`
  = X / Y(height) / Z. (Also the manager copy `[er+0x3D69BA8]+0x70/+0x74/+0x78`.)
- **Player IMMEDIATE-block MapId:** `*(uint32*)([er+0x3D691D8]+0x2C)` →
  `area=(id>>24)&0xff, gridX=(id>>16)&0xff, gridZ=(id>>8)&0xff, lod=id&0xff`.
- **Block→world re-parent transform:** `FUN_1408775e0` (RVA `0x8775e0`): given
  `(MapId, localVec)`, walks the loaded-block RB-tree (node `+0x1C`=MapId key, `+0x20`=parent
  MapId, `+0x24/+0x28/+0x2C`=block origin) and accumulates origins up the parent chain → world.
- **(MapId,local) → map-UI:** `FUN_140876140` (RVA `0x876140`), `CELLSIZE=256.0`
  (`DAT_1429CE8B4`), signZ flip `DAT_14329F470=0x80000000`. This is the converter our
  `worldmap_probe::project()` already calls.

So we **already have** the machinery to turn any `(MapId, block-local pos)` into both world and
map-UI coords. The missing piece is the **enemy ChrIns** equivalent of `[WCM+0x1E508]` + its
pos + its MapId.

---

## 2. Targets (in priority order)

### T1 — Enemy ChrIns enumeration off WorldChrMan (REQUIRED)
Find the WorldChrMan field that lists **all loaded ChrIns** (the open-world enemy/NPC array or
the streamed-entity list), and per ChrIns:
- its **world/block-local position** (the analog of player `+0x6C0`, or the `+0x190…` physics
  chain — `re_findings_playerpos.md` §v19/v20 read player pos via
  `[[[[WCM+0x1E508]+0x58]+0x10]+0x190]+0x68]+0x70/0x78`; check whether the SAME sub-offsets hold
  for a non-player ChrIns);
- its **current-block MapId** (so we can re-parent via `FUN_1408775e0`);
- a stable **identity** to match against `boss_list.json`: NPC param id (`npcParamId`, e.g.
  34510912), model id (`enemyModel`, e.g. `c3451`), or MSB entity id.

Known community anchors to confirm on this build (do not trust blindly):
- WorldChrMan enemy/chr-set list is commonly reached via a `ChrSet` / `EntityList` pointer at a
  fixed WCM offset, each entry → ChrIns. Find the offset by AOB or by CE pointer-scan from a
  live enemy's HP/pos.

### T2 — Live read of ONE dungeon boss (THE decisive experiment)
Pick a loaded dungeon boss with a clean `boss_list.json` entry (e.g. **Stonedigger Troll**,
areaNo=32, m32, internal pos ≈ `(27.704, -0.793)`, or Scaly Misbegotten above). Standing in the
dungeon with it loaded:
1. Read its runtime block-local pos + MapId via T1.
2. Push that `(MapId, local)` through `FUN_1408775e0` (→ world) and `FUN_140876140` (→ map-UI).
3. Compare the resulting map-UI coord to (a) our baked-then-projected marker spot, and (b) the
   spot MapGenie/the player expects (the dungeon entrance/grace).

Report all three coords in map-UI space. **This single observation decides the whole feature
(runtime vs design fix).**

### T3 — How does VANILLA place the field-boss marker? (cheap alternative)
Independently of WorldChrMan: the vanilla `WorldMapPointParam` field-boss row (`textId2==5100`)
ALREADY renders correctly. Determine whether that **row's own `areaNo/gridXNo/gridZNo/posX/posZ`
is the overworld map position** (i.e. the param row is authored in overworld frame, not the
dungeon's internal frame). If yes, the fix is trivial and needs **no RE**: change
`generate_boss_list.py` to bake the **param row position** instead of the MSB entity position.
- Dump the field-boss rows for a few dungeon bosses (Stonedigger Troll etc.) and compare the
  row's `(areaNo,gridX,gridZ,posX,posZ)` vs the MSB entity pos we currently bake. State plainly
  which frame each is in.

### T4 — Unloaded enemies (scope the limitation)
Confirm the obvious: an enemy not currently streamed has **no ChrIns** → no runtime pos. So any
runtime-based fix only works while the player is inside the dungeon. Note this explicitly so the
design decision (A collapse-to-entrance vs B faithful) is made with the constraint in view.

---

## 3. Deliverables (write to `windows_enemy_boss_runtime_pos_re_findings.md`)

1. **T2 verdict first, in one line:** does runtime pos land closer to the entrance than the baked
   value, same, or worse? → "runtime reading WORTH wiring / DEAD END".
2. **T3 verdict:** is the vanilla field-boss param row already in overworld frame? (If yes, this
   likely supersedes the whole WorldChrMan path — flag it.)
3. WorldChrMan → enemy-list offset + per-ChrIns pos sub-offset chain + MapId source + identity
   field, each with the AOB/RVA and a live-read sample value.
4. AOBs for every static/RVA (entry AOB + uniqueness note), in the table style of the other
   findings docs.
5. Any block-tree re-parent gotcha for dungeon blocks (do dungeon MapIds even appear in the
   `FUN_1408775e0` RB-tree, or only overworld blocks? — relevant because dungeon-internal frames
   may not re-parent to the overworld at all).

---

## 4. Notes / pitfalls

- ERR is a regulation mod over ER; param **layouts** match vanilla but **row values** differ
  (boss placements moved). Read params/structs LIVE, never assume vanilla row data.
- `WorldMapLegacyConvParam` is dungeon-grid→overworld-grid only — it does NOT carry the
  boss's intra-block offset, which is the root of the drift.
- Match enemies to `boss_list.json` by `npcParamId` first (most stable across the mod), then
  model id, then MSB entity id.
- Keep the existing player-pos chain untouched; this is additive (a parallel enemy reader).
- If T3 (param-row-is-overworld) holds, prefer it: zero runtime cost, fixes unloaded bosses too.
