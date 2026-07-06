# Native Scaleform enemy names (replace the ImGui overlay path)

**Status:** ★ **MECHANISM SOLVED + native path PROVEN LIVE 2026-07-06 (Windows).** The Scaleform-RE
approach below is **SUPERSEDED** — no gfx RE, no `SetTextHTML` capture needed. The enemy name is drawn by
the **VANILLA ENGINE** (writer RIP in `eldenring.exe`, not `reforged.dll`), from `NpcParam.nameId →
NpcName` FMG, and the engine **re-reads `nameId` LIVE** (no cache-at-spawn). So the mod-agnostic native
solution is a pure **DATA path**: inject a `NpcName` string + write the entity's `NpcParam.nameId` → the
engine shows OUR name in its own stable red tag. **Proven end-to-end on a generic** (an Aigle, nameId=0):
`fmg_set 18 999001 "TESTAIGLE_MFG"` + `param_set NpcParam 60010010 0xc s32 999001` → the native red
`EnemyTag` rendered "TESTAIGLE_MFG". **Remaining = IMPLEMENTATION (Linux takes the lead), not RE.** See
"★ THE SOLUTION" below; RE detail in `docs/re/windows_enemy_name_hud_feed_re_findings.md`.

## ★ THE SOLUTION — native enemy names via the engine's own data path (CONFIRMED)

Drive the engine's `nameId → NpcName` HUD path for entities it leaves blank (`nameId == 0` generics), using
the names OUR resolver already computes. No Scaleform, no gfx edit, mod-agnostic (it IS the engine's path).

**Proven facts (live, ER 2.6.2.0):**
- Writer of the native red `EnemyTag_ColorText` is `eldenring.exe` (vanilla engine); `reforged.dll` has no
  Scaleform/name strings → ERR does NOT feed the tag, it just sets `nameId` on its NPCs.
- `EnemyTag_ColorText` has **no gfx-side gating** (FFDEC: trivial `MovieClip`, no visibility/nameId logic) —
  it displays whatever the engine feeds. The "NPC-only" behaviour is purely the engine's `nameId==0 → no
  feed` (the engine only writes the tag when the resolved name != "", user-observed).
- The engine **re-reads `NpcParam.nameId` per tag-refresh** (changed it on a live, already-spawned Aigle →
  the tag updated on re-acquire). So a runtime edit takes effect without respawn.
- Offsets/tools (all live-verified): **`NpcParam.nameId` @ +0x0c (s32)** (`goblin_item_classify.cpp`
  `npc_team_and_name`); **NpcName base FMG slot = 18** (`fmg_set 18 <id> <text>` works; `kNpcNameSlots =
  {428,328,18}`); the engine looks up `NpcName[nameId]` (our tier-1 mirror: `lookup_text_utf8(nameId +
  700000000)`).

**Implementation sketch (Linux):**
1. In the enemy-bar update (host side, where we already walk the 8 `entityHpBars` → ChrIns → `npcParam`/
   `model`): for each visible bar whose `NpcParam.nameId == 0`, resolve `goblin::enemy_display_name(npcParam,
   model)`. Skip if empty (true nameless → stay vanilla-blank).
2. Allocate a **stable, collision-free NpcName id per npcParamId** (e.g. a reserved high band like
   `kMfgNameBase + npcParamId`; verify the band is unused in NpcName across installs). Cache processed
   `npcParamId`s → inject + set once per type, not per frame.
3. `inject_fmg_entries(NpcName_slot=18, {{id, wname}})` with our resolved name; then write
   `NpcParam.nameId = id` for that row (live param table, +0x0c). The engine then renders it in the native
   red tag for every instance of that type.
4. **Delete the ImGui draw** (`draw_enemy_bar_names` in `goblin_overlay_render.cpp`, the
   `get_enemy_bar_labels`/`EnemyBarLabel` feed, the `cfg_enemyNames*` overlay bits) — this removes the
   overlap + jitter. **Keep** `goblin::enemy_display_name` (still used by the map boss-marker supplement).
5. Verify: stable under abrupt camera swings, accents correct (game font), generics/sheep named, single tag
   (no ImGui dup). On VANILLA: same path (it's the engine's own), generics named too — the mod-agnostic win.

**Open implementation questions / risks:**
- **Id band choice** — must not collide with real NpcName ids (vanilla + the active mod). Pick a reserved
  band and validate at boot.
- **Gameplay side-effects of setting `nameId`** — `nameId` should be display-only, but confirm it doesn't
  alter invader/summon/aggro text logic or a "named enemy" behaviour flag. Low risk; verify.
- **Per-type vs per-instance** — `NpcParam.nameId` is per-npcParamId (shared by all instances of a type),
  which is correct (all Aigles = "Aigle"). If two live entities share a param but need different names
  (rare), the data path can't split them — acceptable.
- **Timing** — inject before the first tag-show for a new type; the engine picks it up on the next
  refresh (re-acquire). A one-time-per-type pass in the bar loop covers it.
- Runtime-only: param edit + FMG injection reset on reboot; the bar-loop pass re-applies each session.

---
_The original Scaleform-RE plan is kept below for history; it is NOT the chosen path (the data path above
supersedes it — simpler, no GFx RE, and the GATE it worried about is moot since the engine, not a shipped
gfx, does the feed)._

## Goal

Draw every enemy's name through the ENGINE's own Scaleform HUD tag (`EnemyTag_ColorText` in the field
HUD movie `01_000_fe.gfx`) instead of our ImGui overlay — for ALL entities (generic mobs, sheep, NPCs,
bosses), not just named NPCs. Then DELETE the ImGui enemy-name path.

**Why.** The ImGui path (`goblin_enemy_names.cpp` + `draw_enemy_bar_names` in `goblin_overlay_render.cpp`)
draws at present-time from the engine's cached `entityHpBars` screenPos, clamped to an on-screen band.
That makes it (a) **jitter/stick on abrupt camera moves** (1-frame desync + edge-clamp), and (b) reliant
on our ImGui font for accents. The engine's Scaleform tag is drawn IN the HUD render pass, frame-synced
with the bar, in the game's own font → **rock-stable + correct accents ("Varré") for free** — exactly
what ERR already gets for named NPCs.

## What we already know (2026-07-06)

- `01_000_fe.gfx` = the field HUD Scaleform movie (`_01_000_FE_fla`). Confirmed symbols:
  `EnemyTag1..EnemyTag7` + `M0EnemyTag0` (= **8 enemy tags**, matches the `entityHpBars[8]` slots),
  `EnemyTag_ColorText_12` (the enemy-NAME text field — the red "White Mask Varré"), `flash.text TextField`,
  `MENU_HP_Base_Enemy`, `MENU_FL_LevelSync_Enemy`, `MENU_FE_NameBase`. It's uncompressed (`GFX\x0b`) →
  `strings` readable.
- ERR ships a modified `mod/menu/01_000_fe.gfx`; the `internals/addons/CenteredStatusBars` variant is a
  6-byte tweak of it. **ERR's HUD names only NPCs** (nameId>0) — generics stay blank (user-confirmed).
- The 8 EnemyTags ARE our 8 `entityHpBars`. We already resolve, live + mod-agnostic:
  `CSFEMAN_SLOT`, the `entityHpBars[8]` layout (0x59F0, stride 0x40, handle/screenX/Y/visible),
  `GET_CHRINS_FROM_HANDLE`, and per-entity `npcParam`/`model` → our tier-1/2/3 name resolver
  (`goblin::enemy_display_name`, `windows_enemy_name_runtime_source_re_findings.md`).
- Scaleform foothold exists: `docs/re/worldmap_native_clip_b3_scaleform_re_findings.md`, the `movieclip`
  RPC + `worldmap_probe` MovieImpl work (M5 cull). Different movie, same GFx machinery.

## ★ The mod-agnostic GATE (decide FIRST — determines whether we can delete the ImGui path)

Prime directive: MapForGoblins must work on ANY mod + vanilla. The ImGui path is unconditionally
mod-agnostic (draws regardless of the active HUD gfx). The native path might NOT be:

- **Q1: On VANILLA, does the `EnemyTag_ColorText` tag DISPLAY when fed a name, or is it hidden for
  non-boss enemies?**
  - If it displays when fed → feeding text via an engine hook works everywhere → **mod-agnostic ✓, no
    gfx edit** → safe to delete ImGui.
  - If vanilla HIDES the tag (visibility gated on boss/nameId) → we'd need to SHIP/inject a modified HUD
    gfx → depends on the active install's gfx, conflicts with other HUD mods (PostureBar, CenteredStatusBars,
    any HUD reskin), breaks on gfx layout changes → **NOT mod-agnostic**. Then: keep ImGui as the
    mod-agnostic fallback and make the native path an additive ERR-only enhancement, OR make ImGui itself
    stable (w2s + drop clamp) and DON'T delete it.

Do not delete the ImGui path until Q1 is answered "displays when fed".

## RE targets (the name-feed hook point)

Find where the engine sets each `EnemyTag` name string per frame, so we can inject OUR resolved name for
entities the engine leaves blank (nameId==0 generics/sheep). Candidate approaches, cheapest first:

1. **Hook the HUD name-set for the tag.** The engine HUD-tick iterates the same 8 `entityHpBars`, gets each
   ChrIns, resolves a name (NpcParam.nameId → NpcName FMG — same as our tier-1), and writes it to the
   Scaleform `EnemyTag_ColorText`. Find that write (Scaleform `GFxValue::SetText` / `Movie::SetVariable`
   on the EnemyTag path, or the CSFeMan HUD-name function near the `entityHpBars` loop). Hook it → when the
   engine's name is empty, substitute `goblin::enemy_display_name(npcParam, model)` (tiers 2/3).
   - Entry hints: xref the Scaleform variable path / `EnemyTag` string in `eldenring.exe`; or find-what-writes
     the tag text field live (Linux runtime FWA RPC); or xrefs to the NpcName FMG getter in a HUD context.
     Anchor off the known `CSFEMAN_SLOT` + `entityHpBars` (0x59F0) — the engine's own naming loop is adjacent.
2. **Hook the nameId/name getter in the HUD context** so nameId==0 entities return our name → the engine's
   existing display path renders it. (Same as ERR's likely `hooks\text` style.)
3. **gfx edit** (only if Q1 = hidden): unhide/duplicate the EnemyTag ColorText for all bars in a shipped
   HUD gfx. Invasive, mod-specific — last resort, and itself needs the GFx/menu.bnd authoring path RE'd.

This RE is a Windows-Ghidra (static xref on `D:\ghidra_proj2\ER`) OR Linux-runtime (boot ER + FWA/probe on
the tag write) job. Write findings to `docs/re/windows_enemy_name_hud_feed_re_findings.md`.

## Implementation steps (after RE)

1. Resolve the tag-name-set hook point + the Scaleform movie/tag handle for slot i (reuse CSFeMan +
   `entityHpBars` we already walk).
2. Per visible bar with an engine-empty name, feed `enemy_display_name(npcParam, model)` into the tag
   (its ColorText), letting the engine render it. Keep it mod-agnostic (no baked gfx dependency if Q1 allows).
3. Verify live: stable under abrupt camera swings (the whole point), accents correct, sheep/generics named,
   no dup with ERR's own NPC tags (engine owns the single tag now — no ImGui second copy).
4. **Only then** delete the ImGui path: `draw_enemy_bar_names` (`goblin_overlay_render.cpp`), the
   `get_enemy_bar_labels`/`EnemyBarLabel` render-feed, and the `cfg_enemyNames*` overlay bits — keep
   `goblin::enemy_display_name` (the map boss-marker supplement still uses it). Keep BOTH builds green.

## Non-goals / risks

- If Q1 = "vanilla hides the tag", the mod-agnostic clean answer may be to KEEP ImGui and just fix its
  stability (per-frame w2s of the enemy world pos + drop the `kClampL/R/T/B` band) + the accent fix
  (already landed, `d23b779`). That gets ~ERR stability without any gfx dependency. Fall back to this.
- HUD-gfx dependency = conflicts with other HUD mods; avoid shipping a gfx replacement if at all possible.
- Accent fix (`d23b779`) already helps the ImGui path regardless of which route wins.
