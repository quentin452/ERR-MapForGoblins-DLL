# Run tracker (deaths / in-game time / boss checklist)

Status: **shipped (v1)** — two surfaces, both in `src/overlay_panel/panel_run.cpp`:

- **In-game HUD** (the one meant for play): one compact line, own key (**F10**, `run_hud_key`),
  `ImGuiWindowFlags_NoInputs` → click-through, no cursor grab, no nav focus. Own per-frame render
  entry (`draw_run_hud` → `MFG_DrawRunHud`), so it draws with the F1 panel CLOSED.
- **F1 ▸ Run tab**: the detailed view (counters + per-region boss checklist + the HUD settings).
  Also drawn in the flat list when the settings-search filter is active.

The split exists because F1 is the wrong surface for run info: it takes the cursor and covers the
screen, so it cannot be read mid-fight (user feedback, 2026-07-27). Do not "simplify" by folding
the HUD back into the panel.

## Why it lives in MapForGoblins

Decided 2026-07-27. The alternative was a standalone tracker mod (the sibling
`ER-DeathCounter-Mod`, which already read the death count) growing a boss list of its own. That
loses twice:

- A standalone tracker has to ship a **baked vanilla boss list** to draw a checklist — exactly what
  EROverlay does (`src/boss/data/<lang>/bosses.json`, generated offline from vanilla FMGs). Wrong
  names, wrong flags, wrong count on ERR or any other mod. Against the mod-agnostic prime directive.
- Doing it right means porting `enemy_display_name` + the disk MSB scan + params + FMG + EMEVD out
  of this repo — ~4000 lines, i.e. half this mod's data stack — plus a second Present hook in the
  process (the multi-mod hook war, see `docs/memory/bugs/multimod-hook-coexistence.md`).

MapForGoblins already owns every ingredient but the two save counters, which are ~30 lines. So the
tracker moved here instead, and the standalone death counter becomes redundant.

## Data sources

| Shown | Source |
|---|---|
| Deaths | `GameDataMan + 0x94` (u32) |
| In-game time | `GameDataMan + 0xA0` (u32, **milliseconds**) |
| Bosses defeated / total | the `WorldBosses` marker bucket, deduped by `Marker::cleared_flag` |
| Per-boss defeated state | `overlay_api::read_event_flag(cleared_flag)` |
| Boss name | `Marker::live_name` (mod-agnostic bosses) else `lookup_text_utf8(name_id)` |
| Region grouping | `Marker::loc_pname` → `lookup_text_utf8` |

`goblin::inventory::read_run_stats()` (host side, `goblin_inventory.cpp`) reuses the **existing**
cached `GameDataMan` static slot resolved from the `GAME_DATA_MAN` AOB for the equip chain — no new
signature, no new resolve path. Both reads are RPM-guarded and return false on the title/loading
screen; the panel then keeps the last good values rather than showing 0.

Both offsets are the pair `soarqin/EROverlay` reads (`src/boss/data.hpp`, `kDeathCount` /
`kInGameTime`); `+0x94` is independently proven here by the death-marker path. They are engine SAVE
data, not param/FMG content, so they are mod-agnostic for free.

## The defeat flag — was ERR-only, now mined from EMEVD (2026-07-27)

`Marker::cleared_flag` comes from `WorldMapPointParam.textDisableFlagId1` on boss rows
(`textId2 == 5100`), which is an **ERR encoding vanilla does not use**. Bosses seeded
mod-agnostically by `build_live_bosses` (from the game's own boss health bar, tier 4) build a
synthetic param row and therefore carry **no cleared flag** — `map_entry_layer.cpp` ~L520.

Consequence: on ERR the checklist is complete; on vanilla / another mod the names are right but the
checkmarks are missing. The panel counts those markers separately ("N boss marker(s) carry no defeat
flag on this install") instead of reporting them as alive — do not "fix" that by defaulting them to
not-defeated, it would silently under-report a finished run.

**Measured 2026-07-27, first live test** — this is not a corner case, it is the DEFAULT on a
non-ERR install. The test install logged:

```
[BOSSLIVE] built 0 boss markers from live WorldMapPointParam (textId2==5100)
           + 235 enemy-supplemented instances (222 boss types total,
             222 seeded mod-agnostically from the game's own boss health bar)
```

Zero WMP boss rows → zero cleared flags → the panel showed `0/0` with 235 markers "state unknown".
The boss half of the tracker is therefore **inert on any install without ERR-style boss pins**, and
closing the gap below is a prerequisite, not a nice-to-have.

### The fix (landed, not yet verified in-game)

The lead was right, and the instruction is one id away from the one already parsed:

| | |
|---|---|
| `2003[11]` `DisplayBossHealthBar(entity, slot, nameId)` | already mined → the boss NAME |
| `2003[12]` `HandleBossDefeatAndDisplayBanner(entity, bannerType)` | now mined → the boss DEFEAT |

**The defeat flag id IS the entity id.** 2003[12] takes no flag argument because the engine sets
the flag whose id equals the entity, and ER's own events read it back that way — e.g. Godrick
(`10000800`) is gated on `EventFlag(10000800)`. Measured over the decompiled corpus: **134 of the
140** entities passed to 2003[12] are re-read as `EventFlag(<same id>)`.

Wiring: `msbe::parse_emevd_boss_defeats` (msbe_parser.cpp) → collected by the SAME EMEVD walk that
mines the boss-bar names (`emevd_boss_bars()`, no second Oodle pass) → `emevd_boss_defeat_flag()` →
`build_live_bosses` sets `d.clearedEventFlagId` on each mod-agnostically supplemented boss.

Returning 0 for an entity with **no** 2003[12] registration is deliberate: its flag would never be
set, so treating it as a valid flag would report a beaten boss as alive forever. Those stay in the
"state unknown" bucket.

**Verified in-game on VANILLA, 2026-07-27:** `[LOOTDISK] boss defeats: 215 entities registered
(from 451 2003[12] calls)`, markers carry 201 distinct defeat flags, panel reads `6/201` on a
played save. The mod-agnostic path works with no ERR param rows at all.

### Reading the three counts (they are SUPPOSED to differ)

| Count | Meaning |
|---|---|
| 251 | entities with a health bar — `2003[11]` |
| 215 | entities with a defeat registration — `2003[12]` |
| 201 | distinct defeat flags actually attached to a boss marker |

**251 → 215 is correct behaviour, not loss.** Measured over the decompiled corpus: defeat entities
end in `x800` (114) or `x850` (17) — the *primary* body of a fight; the bar-only ones end in `x801`
(22), `x802` (4), `x851/852/853` (9), `x810-813`… — the *secondary bodies* of multi-part fights
(Godskin Duo, Crucible Knight + Misbegotten, the Gargoyles). **48 of the 51 bar-only entities have
their `x800`/`x850` primary in the defeat set.** The remaining ones are degenerate: entity `0`, and
`10000` = the PLAYER (a boss bar drawn on the player). Since the tracker's unit is the FIGHT, giving
companions their own flag would make the Godskin Duo count as 2 — do not "fix" this gap.

**215 → 201 was ours, and is now gone — the tracker no longer sources its list from markers.**
Measured 2026-07-27: `[BOSSLIVE] defeat flags: 201 of 215 registered entities are on a marker; 14
unmatched — 15000800, 20010850, 30130810, 31000850, 32080800, 34100800, 34110800, 34150800,
1037530800, 1041330800, 1052380800, 1052520800, 1054560800, 2050430710`. `15000800` is **Malenia**.

Those 14 are the losses HANDOFF (2026-07-27, line 21) already accounted for on the marker side:
`251 − 6 (LOD-tile only) − 5 (not MSB Enemy parts) = 240 reachable`, then the per-(type,tile) dedup.
Every one of those steps is right for DRAWING and wrong for COUNTING — a marker is a map-display
object.

So `rebuild()` enumerates `emevd_boss_defeat_entities()` (the engine's own registrations) and uses
markers only to enrich a row with its name/region, joined on `cleared_flag == entityId`. A fight
with no surviving marker still counts, and still gets a name via
`goblin::boss_bar_display_name(entityId)` (tier 4 from the entity alone, no NpcParam/model needed).

**Rule: the run tracker's unit is the engine's defeat registration, never the marker.** If a future
change makes the checklist read from markers again, Malenia silently disappears from the total.

## Cross-check vs EROverlay's curated list (2026-07-27) — 215 vs 207

Reconciled our EMEVD `2003[12]` set (vanilla install, `[LOOTDISK] boss defeat ids:` dump) against
`soarqin/EROverlay`'s baked `bosses.json` (207 entries, whose `flag_id` is an entity id too, so the
sets compare directly). **193 shared, 22 only ours, 14 only theirs.** Four distinct causes:

> **Causes 1 and 4 are still OPEN.** The first attempt (mining the common-func call sites with the
> handler ids read off an ERR decompile) MISSED — see the post-mortem below. Re-run the diff after
> any further attempt; this section records the measurement that found the defects.

**1. Same fight, different id — 10 cases. WE ARE WRONG HERE (open defect).**
Night/roaming bosses (Death Rite Bird, Night's Cavalry ×3, Deathbird ×2, Tibia Mariner) plus two
DLC NPCs (Dryleaf Dane, Count Ymir). The engine passes the `x340`/`x710`/`x720` entity to
`2003[12]`; EROverlay uses the tile's `x800`. **Ours is a TRANSIENT flag**: `common.emevd` explicitly
resets it — `SetEventFlagID(1043370340, OFF)` — while real boss flags are absent from that reset
list (verified for `10000800` Godrick, `15000800` Malenia, `1042360800`, `1052380800` Radahn). So
those 10 would un-tick themselves.
*Fix lead:* the two ids are joined in the EMEVD itself —
`InitializeCommonEvent(0, 90005860, 1043370800, 0, 1043370340, 0, 1043370400, 0)` binds the
persistent flag to the entity. Mining `90005860` (same technique as the `90005702` quest-NPC mine)
gives entity → persistent flag. NOT done yet. Do NOT instead reject "ids in the reset list": 21
legitimate `x800` flags are in it too.

**2. `12`-prefixed twin — 3 cases.** Radahn `1052380800` (ours) vs `1252380800` (theirs), plus
`1052520800` and Borealis `1054560800`. Their id is the one other events actually read
(`EndIf(EventFlag(1252380800))`, and the demigod list in `common.emevd`); ours is the character
entity. Both mark the same kill and each list counts the fight once, so the totals are unaffected —
but which of the two the engine actually SETS is unverified. Only 3 of 78 overworld ids have such a
twin, so this is not a systematic convention.

**3. Fights their curated list omits — 12.** `20010850`, `30130810`, `31000850`, `34100800`,
`34110800`, `34150800`, `1034500800`, `1037510800`, `1041330800`, `2050430710`, and the two
above. The engine registers a defeat for these; a hand-curated vanilla list simply does not carry
them. **This is the baked-list drift the no-bake directive exists to avoid** — our source is right.

**4. One we miss:** `1041510800` Tree Sentinel(×2), Altus. Suspected parser limitation: `2003[12]`
invoked from a `common_func` with **parameterized** args, where the instruction's arg bytes are
placeholders rather than literal entity ids — `parse_emevd_boss_defeats` only reads literals.
Unverified; the same blind spot would hide any other parameterized registration.

Repro: `[LOOTDISK] boss defeat ids:` in the log + the scratchpad `diff_bosses.py`.

### The fix for causes 1 + 4 — mine the common-func CALL SITES

Both defects have one root: `common_func.emevd` registers boss defeats as
**`HandleBossDefeatAndDisplayBanner(X8_4, banner)`** inside four shared handlers — **`9005840`,
`90005860`, `90005861`, `90005880`** — so the entity arrives as a PARAMETER. The literal instruction
scan sees a placeholder, which is why the Altus duo was invisible. The information is at the call
site instead:

```
InitializeCommonEvent(0, 90005860, <X0_4 = persistent flag>, 0, <X8_4 = entity>, 0, ...)
                          a+4          a+8                        a+16
```

Measured on the corpus: **102 call sites, 93 entities**; `X0_4` is `x800`/`x850` in 85 of them while
`X8_4` is `x340` in 8 — i.e. the call site hands over BOTH the fight's persistent flag and the
entity whose death raises the banner. So `parse_emevd_boss_defeat_calls` mines the call sites, and
`defeats()` became a **map entity → flag** merged from two sources:

| source | insert | why |
|---|---|---|
| literal `2003[12]` | `emplace(e, e)` — never overwrites | flag = entity, the common case |
| common-func call site | `defeats()[entity] = X0_4` — **wins** | only source for param'd handlers, and its flag is the persistent one |

Consequences to keep in mind (the plumbing landed and is correct — only the handler ids were wrong):
- **A fight's flag is no longer always its entity id.** Look names up with the ENTITY
  (`boss_bar_display_name`), and state with the FLAG. Mixing them silently breaks the 10.
- **Distinct FLAGS = distinct fights**, not distinct entities: two bodies can share one flag, so the
  panel and the coverage diagnostic both dedup on the flag.

### Why the call-site pass looks like it did nothing (it didn't fail)

Measured on the vanilla install right after shipping the above:

```
[LOOTDISK] boss defeats: 216 entities -> 216 distinct fights (from 451 literal 2003[12] calls
           + 1 common-func call sites; 0 entities carry a flag that is NOT their own id)
```

**1 call site, 0 re-flagged** — but nothing is broken. The handler-discovery probe answered it:

```
[LOOTDISK] defeat handlers in common_func.emevd.dcx:
    9005840(entity=0), 90005860(entity=0) x4, 90005861(entity=0) x4, 90005880(entity=0)
```

**Vanilla ships the SAME four handlers** (`entity=0` = the entity really is parameter-substituted),
so the ids were never ERR-specific. And the mechanism works: the sibling parser finds
`139 90005702 calls` over the same 589 files with the identical bank + arg layout.

The difference is usage, and the two numbers agree: **vanilla inlines its boss defeats** (451 literal
`2003[12]`, ~1 handler call site) while **ERR refactored them into the shared handlers** (175 literal,
102 call sites). So the call-site pass is correct and necessary — it is simply near-empty on vanilla
and will carry real weight on ERR-like installs. Its one vanilla hit raised the total 215 → 216.

**Consequence for the night/roaming bosses: on vanilla the call site cannot fix them.** They reach
`2003[12]` literally with their transient `x340` entity, and there is no handler invocation carrying
the persistent `x800`. That defect needs a different vanilla-side source — the event that sets the
`x800` flag on their death — and remains OPEN.

Keep `parse_emevd_defeat_handlers()` anyway: it is what turned a wrong guess ("our ids are ERR's")
into a measurement, and it is the check to re-run on any install where the counts look off.

## Not done (deliberate)

- **Remembrances obtained.** The natural source is `EquipParamGoods` + carried inventory
  (`goblin::inventory::goods_count`), mirroring how Great Runes are already resolved by
  `goodsType == 15` (`map_entry_layer.cpp` `build_great_rune_markers`). The remembrance `goodsType`
  has NOT been confirmed live — do that before writing the code.
- **Challenge mode** (EROverlay stops recording kills past N deaths, tracks PB / tries). Needs
  persisted per-attempt state; no home for it yet.
- **Reviving a boss** (EROverlay lets you clear a defeat flag from its checkbox). This panel is
  read-only on purpose — flag writes belong behind the existing `questAllowFlagWrite`-style gate.

## Cost / cadence

`rebuild()` walks every marker of every layer and hits the flag reader once per DISTINCT encounter
(not per marker — a boss type can own hundreds of markers). Throttled to 1 s while the section is
visible, plus a Refresh button. The two save reads run per drawn frame (2 guarded RPM calls).

## Wiring

- Host: `goblin::inventory::read_run_stats()` marked `GOBLIN_RENDER_API` (render→host direction,
  fix pattern 1 in `CLAUDE.md`). Both builds verified green: `build-err` (shipped single DLL) and
  `build-err-hotreload` (`MapForGoblins` + `goblin_overlay_render`).
- The **Run** tab is registered LAST (index 5) so the `f1_tab` RPC indices 0-4 keep pointing at
  Markers / Search / Quests / Display / Dev.
