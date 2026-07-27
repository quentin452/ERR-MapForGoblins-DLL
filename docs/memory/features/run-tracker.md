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

**Still to verify in-game (vanilla):** that the count is non-zero and that killing a boss ticks it.
Check `[LOOTDISK] boss defeats: N entities registered` in the log first — N ≈ 140 on vanilla.

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
