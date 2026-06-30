# Implementation Plan: Quest Browser Automation & NPC Map Display (v2)

This plan automates the Quest Browser (reflecting — and optionally writing — Event Flags
in real time) and dynamically shows the NPC/asset of the active quest step on the world
map.

> **v2 note.** This revision was rewritten after an audit found the v1 plan ignored
> infrastructure already shipped on `master`, duplicated an existing entity→position
> index, and treated event-flag *writing* as a free action. The codebase ALREADY has:
> a `WorldQuestNPC` category emitted by `map_entry_layer.cpp` (gated by the
> `questNpcQuestAware` "quest-aware" filter in `map_renderer.cpp`); a complete Quest
> Browser; and per-step progress persisted in `config::questProgress` (a blob keyed by
> NPC name, read/written by `qp_get`/`qp_set` in `goblin_overlay.cpp`). v2 *replaces* the
> legacy path rather than adding a parallel one, reuses the existing prebuild index, and
> defaults flag handling to **read-only**.

---

## 0. Existing Infrastructure To Reconcile (READ FIRST)

Before writing any new code, the implementer must account for what already exists, or the
feature will double-draw markers and fork the progress source of truth.

| Concern | Already in tree | v2 decision |
| --- | --- | --- |
| Quest-NPC map category | `Category::WorldQuestNPC`, emitted in [map_entry_layer.cpp](../src/worldmap/map_entry_layer.cpp) (~L1891); meta in [category_meta.cpp](../src/worldmap/category_meta.cpp) (~L87) | **Retire** the legacy emission; the new `QuestNpcLayer` becomes the sole producer of `WorldQuestNPC` markers. |
| Quest-aware gate | `config::questNpcQuestAware` + `is_quest_npc_hidden()` in [map_renderer.cpp](../src/worldmap/map_renderer.cpp) (~L985); `quest_aware()`/`set_quest_aware()` in [goblin_inject.cpp](../src/goblin_inject.cpp) (~L4217) | Keep the toggle, but it now gates the new layer. The overlay already labels the legacy pins "legacy / unfinished — superseded by the Quest Browser"; that wording goes away once the layer is real. |
| Per-step progress | `config::questProgress` blob keyed by NPC name; `qp_get`/`qp_set` lambdas in [goblin_overlay.cpp](../src/goblin_overlay.cpp) (~L2608) | Keep the ini blob as the **manual** layer; flags become an optional *reflection* on top (see §4). |
| Entity → position index | `ent_enemy` / `ent_any` maps built once in `prebuild_markers()` ([map_entry_layer.cpp](../src/worldmap/map_entry_layer.cpp) L722–742) | **Reuse** via an exposed lookup function — do NOT add parallel global maps. |
| Flag read/write helpers | `goblin::ui::read_event_flag(id)` ([goblin_inject.cpp](../src/goblin_inject.cpp) L4390); `goblin::markers::set_event_flag(id,val)` ([goblin_markers.cpp](../src/goblin_markers.cpp) L133) | Read freely; **write only behind an explicit opt-in cheat gate** (see §4). |

---

## 1. Blind Spots & Special Cases (unchanged from v1 — still valid)

### 1a. NPC Hostility and Absolution (the "Ranni" effect)
If the player attacks Ranni (or another NPC), she and her servants (Blaidd, Iji, Seluvis)
vanish until absolution at the Church of Vows (Celestial Dew). The EMEVD disables those
entities via a faction-hostility flag. Because the marker layer keys off the same live
activation state, **the NPCs disappear from the map automatically** — correct behavior.
* **UX:** an optional `hostility_flag` on `NpcQuest`. When set in memory, the Quest
  Browser shows an amber note `[Hostile — obtain absolution at the Church of Vows]`. This
  complements the existing `questGreyOnDeath` / `fail_flag` machinery rather than
  replacing it.

### 1b. Transition to a boss fight (Blaidd, Alexander, Millicent…)
When an NPC becomes a permanent boss or dies, its death / boss-transition flag is set; the
step is marked done/failed and the friendly marker drops — this rides on the existing
`fail_flag` / `fail_conclusion` fields.

### 1c. Time-of-day NPCs (e.g. Renna at the Church of Elleh)
Physical activation depends on the day/night cycle. We keep the marker driven by the
active *quest-step flag* (a "go here" indicator), not strict physical activation, so the
pin shows regardless of in-game time.

---

## 2. Quest Metadata

#### [MODIFY] [src/generated/goblin_quest_steps.hpp](../src/generated/goblin_quest_steps.hpp)
Additive, trailing default-initialised members (same pattern as the existing
`warning` / `fail_flag` / `fail_conclusion` additions — unspecified entries default to 0).

```cpp
struct QuestStep {
    const char *title;
    const char *desc;
    const char *zone;
    uint32_t progress_flag = 0; // EMEVD flag whose SET state means this step is done. 0 = manual-only.
    uint32_t entity_id    = 0;  // MSB EntityID of the NPC/asset to pin for this step. 0 = no marker.
};

struct NpcQuest {
    const char *name;
    const char *quest_title;
    const char *related;
    const QuestStep *steps;
    size_t step_count;
    bool dlc = false;
    const char *warning = nullptr;
    uint32_t fail_flag = 0;
    bool fail_conclusion = false;
    uint32_t name_id = 0;        // FMG NPC name id (tooltip label). 0 = use `name`.
    uint32_t hostility_flag = 0; // amber "obtain absolution" note when set. 0 = none.
};
```

#### Data acquisition — the real cost (do NOT hand-bake)
The audit's main finding: the feature's cost is the **data**, not the code. The repo's
whole trajectory is no-bake / single-source-of-truth, so `progress_flag` and `entity_id`
must be **derived**, not hard-coded long-term:
* `entity_id` → the friendly-NPC entity is already discoverable from the parsed MSB Enemy
  parts (the same source the disk passes use); join to position via the prebuild index
  (§3). The NPC↔quest association can be cross-referenced from the EMEVD that activates the
  questline.
* `progress_flag` → harvest from EMEVD with the existing DarkScript3 decompile tooling
  (see the `darkscript3-emevd-decompile` notes) and/or the in-overlay **event-flag hook**
  (already wired as a coverage-gap detector) while playing the step.
* **Bootstrap exception:** it is acceptable to hand-author a small demo set (Boc,
  Alexander, Thops) in [goblin_quest_steps.cpp](../src/generated/goblin_quest_steps.cpp)
  to prove the pipeline end-to-end — but the plan must land a *generator/derivation* path
  for the full 34 questlines, mirroring `tools/generate_*.py`. Tag any hand-authored row
  so a later derivation pass can replace it.

---

## 3. Entity → Position Lookup (REUSE, don't duplicate)

#### [MODIFY] [src/worldmap/map_entry_layer.cpp](../src/worldmap/map_entry_layer.cpp) / [.hpp](../src/worldmap/map_entry_layer.hpp)
`prebuild_markers()` already builds `ent_enemy` (entity→DiskEnemy) and `ent_any`
(entity→enemy-or-asset) at L722–742. Rather than adding parallel
`g_enemies_by_entity_id` / `g_assets_by_entity_id` globals, **promote the existing index
to a file-scope cache and expose a lookup**:

```cpp
// map_entry_layer.hpp
namespace goblin::worldmap {
// Resolve an MSB EntityID to a world position from the prebuilt disk index.
// Returns false if unknown (entity not placed / index not built yet).
bool entity_world_pos(uint32_t entity_id, float &worldX, float &worldZ, int &group);
}
```

The lookup serves enemy first, then asset (matching the existing `ent_enemy`→`ent_any`
precedence). No data is cleared on teleport, so the global-scope concern from v1's test
plan is moot — the index is built once per map load.

---

## 4. Quest Browser flag handling — READ-ONLY by default (the risky part)

Writing EMEVD progress flags **mutates the save** and can soft-lock a questline, skip a
reward, or trigger an unintended event (many of these flags are set by the engine *after*
a dialogue/cutscene, with side effects we don't reproduce). v2 therefore splits behavior:

* **Reflection (default, safe):** if a step has a `progress_flag`, `qp_get` returns the
  live flag via `read_event_flag()`. The checkbox renders as a **read-only mirror** —
  ticked when the flag is set, with an `[auto]` tag. The manual `questProgress` ini bit is
  ignored for that step (flag wins). No writes occur.
* **Writing (opt-in cheat):** a new `config::questAllowFlagWrite` (default **false**,
  persisted), surfaced in the Quest Browser as `Allow writing quest flags (cheat — can
  break quests)`. Only when ON does the checkbox become editable and call
  `set_event_flag()`. Hover tooltip spells out the soft-lock risk and recommends a
  throwaway save.
* **Source-of-truth priority:** flag-backed step → flag is truth (read-only unless the
  cheat gate is on). Flag-less step (`progress_flag == 0`) → unchanged manual ini bit.
  This keeps a clear per-step rule instead of a silent hybrid.

#### [MODIFY] [src/goblin_overlay.cpp](../src/goblin_overlay.cpp) (Quest Browser, ~L2608+)
* `qp_get(name, s)`: if `steps[s].progress_flag` → `read_event_flag(flag)`; else existing
  ini-bit read.
* `qp_set(name, s, v)`: if `progress_flag` → only act when `questAllowFlagWrite`, then
  `set_event_flag(flag, v)`; else existing ini-bit write.
* Render `[auto]` tag + risk tooltip on flag-backed steps; disable the checkbox when
  writes are off.
* If `hostility_flag` is set in memory, show the amber `[Hostile — obtain absolution…]`
  note on the questline header (next to the existing `[unfinishable]`/`(!)` tags).

---

## 5. Map Rendering — replace the legacy emission with a layer

#### [NEW] [src/worldmap/quest_npc_layer.hpp](../src/worldmap/quest_npc_layer.hpp)
```cpp
#pragma once
#include "marker_layer.hpp"

namespace goblin::worldmap
{
class QuestNpcLayer : public MarkerLayer
{
public:
    const char *category() const override { return "World - Quest NPC"; }
    bool visible() const override;                       // master gate + questNpcQuestAware
    const std::vector<Marker> &markers() const override; // returns the cached vector

    void invalidate();                                   // force a rebuild next markers()
private:
    mutable std::vector<Marker> cache_;
    mutable uint64_t built_epoch_ = ~0ull;               // rebuild guard
};
}
```

#### [NEW] [src/worldmap/quest_npc_layer.cpp](../src/worldmap/quest_npc_layer.cpp)
* For each questline, find the first step not done (per §4's `qp_get` rule).
* If that step has an `entity_id`, resolve its position via `entity_world_pos()` (§3);
  build one `Marker { category = WorldQuestNPC, icon_key = "show_quest_npc", name_id }`.
* **Cache invalidation (the v1 gap):** the active step changes when a flag flips, so the
  cache cannot be built once. Rebuild when *any* of these changed since `built_epoch_`:
  the set of relevant progress-flag values, `questProgress`, `questNpcQuestAware`, or the
  loaded map. Cheapest correct approach: rebuild on map-open and whenever the overlay
  observes a quest-flag/`questProgress` change (the overlay already polls flags for the
  page gates); otherwise serve the cache. Do **not** re-read every quest flag per frame in
  the marker hot loop — see the `overlay-render-perf-followups` perf notes.

#### [MODIFY] [src/worldmap/map_entry_layer.cpp](../src/worldmap/map_entry_layer.cpp)
* **Remove** the legacy `WorldQuestNPC` marker emission (~L1891) — the layer is now the
  sole producer, preventing double-draw.

---

## 6. Overlay wiring

#### [MODIFY] [src/goblin_overlay.cpp](../src/goblin_overlay.cpp)
* Register `QuestNpcLayer` in the active-layers list.
* Drop the "legacy / unfinished" tooltip wording on the `World - Quest NPC` category row
  (it is no longer legacy). Keep the `questNpcQuestAware` toggle as the layer's gate.
* No change needed to the generic `s_cat` loop *unless* the layer and the loop both try to
  own `WorldQuestNPC`; since §5 makes the layer the sole producer, confirm the category is
  driven by the layer and not re-added by the static path.

---

## 7. Verification Plan

### Build
1. Build the DLL with the project toolchain (clang-cl + ninja per `build-toolchain-clang-xwin`).

### Functional
2. **No double-draw:** with a flag-backed demo quest active, confirm exactly ONE
   `WorldQuestNPC` marker (legacy path retired).
3. **Position resolve (enemy & asset):** one demo step pinning an Enemy entity, one pinning
   an Asset entity (e.g. Ranni's doll) — both place correctly. Teleport across regions and
   confirm the index is not cleared.
4. **Read-only reflection:** with `questAllowFlagWrite` OFF, advance a quest in-game and
   confirm the `[auto]` checkbox ticks itself from the live flag and is NOT editable.
5. **Hostility note:** anger Ranni, confirm the amber absolution note appears and the
   markers drop.

### Safety (the v1 gap)
6. **Save-mutation test on a THROWAWAY save:** enable `questAllowFlagWrite`, toggle a
   `progress_flag`, then verify in-game the questline still advances coherently (no
   soft-lock, dialogue/reward not skipped). If a flag is unsafe to force, leave it
   read-only (omit it as a writable step). Never test flag writes on the user's real save.

### Perf
7. Confirm the marker hot loop does not re-read all quest flags per frame (cache served;
   rebuild only on the §5 triggers).

---

## Appendix — salvaged NPC curation (from the retired `feat/quest-npc-layer` branch)

The old static generator (`tools/generate_quest_npcs.py`, baked approach) is superseded by this plan's
runtime `QuestNpcLayer` (entity-id / MSB-Enemy / EMEVD driven). Before deleting that branch, its
hand-curated NPC-vs-noise knowledge is preserved here — useful as a **denylist / sanity filter** if the
runtime entity selection surfaces false positives.

- **Friendly / quest NPC `teamType` set** (`NpcParam.teamType`): `{0, 1, 2, 26, 28}` — 0/1/2 = friendly
  generics, 26 = quest/merchant NPCs, 28 added during ERR tuning. Hostile/enemy teamTypes to EXCLUDE
  seen during inspection: 6 (Crucible/field), 7 (great enemy), 24/27 (invaders), 33/48/9/52 (misc enemy).
- **`_LABEL_PREFIXES`** to strip (placed-asset labels that aren't quest NPCs):
  `('Hub:', 'Tutorial:', '???:', 'Legacy Dungeon:', 'Minor Dungeon:', 'Dungeon:', 'Underground:',
  'Evergaol:', 'Field Area:', 'Field Boss:')`
- **`EXCLUDE_NAMES`** — curated non-quest names that otherwise pass the teamType filter (captured in
  full here since the source branch is being deleted):
  - placeholders / structures / object markers (mostly team 0): `Menu`, `Altar of Anticipation`,
    `Clouded Mirror Stand`, `Sword of Bernahl`, `Basin of Atonement`, `Church of Dragon Communion`,
    `Cathedral of Dragon Communion`, `Grand Altar of Dragon Communion`, `Smithing Table`,
    `Smithing Anvil`, `Trial of Recollection`, `Current Run`, `Weapon Chest`, `Spirit Monument`,
    `Lord's Journey`, `Torrent`, `Jar-Friend's Bazaar`.
  - enemies/bosses leaking into the friendly teamTypes (mostly team 2 / team 0):
    `Swordhand of Night Jolán`, `Equilibrious Rat`, `Old Knight Istvan`, `Great Horned Tragoth`,
    `Rennala, Queen of the Full Moon`, `Tanith's Knight`.

Note: the branch ALSO lacked master's id-based filtering (`DENY_NAME_IDS` / `FORCE_NAME_IDS` /
`EXCLUDE_MODELS`, 9 rules in `tools/generate_quest_npcs.py` on master). If any baked-generator path is
ever revived, the two filter layers (master = by id/model, branch = by name/team) are complementary and
should be merged, not chosen between.
