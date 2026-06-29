# Implementation Plan: Quest Browser Automation & NPC Map Display

This plan describes the changes required to automate the Quest Browser (reading/writing Event Flags in real-time) and dynamically display the position of the NPC or asset corresponding to the active step on the world map.

---

## Blind Spots & Special Cases Identified

### 1. NPC Hostility and Absolution (The "Ranni" Effect)
* **The Problem:** If the player attacks Ranni (or another NPC), Ranni and her servants (Blaidd, Iji, Seluvis) disappear from the world. The player must obtain absolution at the Church of Vows using Celestial Dew to make them reappear.
* **Map Impact:** The EMEVD handles this disappearance by temporarily disabling the entities via a faction hostility flag (e.g., Ranni's faction becomes hostile). Since our `QuestNpcLayer` uses the actual activation state (which depends on these same hostility flags), **the NPCs will automatically disappear from the map** when the player commits a sin. This is the expected and correct behavior.
* **Quest Browser UX/DX Improvement:** To prevent the player from being confused about why NPCs disappeared without the quest being marked as failed:
  * We can define an optional `hostility_flag` on the quest (`NpcQuest`).
  * If this flag is active in memory, the Quest Browser will display an orange warning message: `[Hostile - Obtain absolution at the Church of Vows]`.

### 2. Transition to Boss Fights (e.g., Blaidd, Millicent)
* **The Problem:** At the end of certain quests, the NPC becomes a hostile boss (e.g., crazed Blaidd in front of Ranni's Rise, or the duel against Alexander).
* **Solution:** Once the NPC becomes permanently hostile or dies, their death flag or boss transition flag is activated. The corresponding quest step is marked as completed or failed, which automatically removes the friendly NPC marker from the map.

### 3. Time-of-Day Dependent NPCs (Time of Day)
* **The Problem:** Some NPCs only appear at specific times (e.g., Renna at the Church of Elleh only at night at the very beginning of the game).
* **Solution:** The physical activation of the entity depends on the game's day/night cycle. If the player looks for the NPC during the day, the icon on the map can either remain displayed (as a "meeting point" indicator) or be hidden if we rely solely on strict physical activation. We will keep the display based on the active quest step flag so the player knows where to go, regardless of the time.

---

## Proposed Changes

### 1. Component: Quest Metadata

#### [MODIFY] [goblin_quest_steps.hpp](file:///home/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL/src/generated/goblin_quest_steps.hpp)
Add members to structures for progressive automation (unspecified fields in `.cpp` will default to `0`).

```cpp
struct QuestStep {
    const char *title;
    const char *desc;
    const char *zone;
    uint32_t progress_flag = 0; // 0 = manual
    uint32_t entity_id = 0;     // 0 = no map marker
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
    uint32_t name_id = 0;        // FMG NPC Name ID (e.g., 122300 for Boc)
    uint32_t hostility_flag = 0; // Hostility flag (e.g., angry Ranni). 0 = none.
};
```

#### [MODIFY] [goblin_quest_steps.cpp](file:///home/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL/src/generated/goblin_quest_steps.cpp)
Add flags and entity IDs for a few target quests for demonstration (e.g., Boc, Alexander, Thops).

---

### 2. Component: Global Position Indexing

#### [MODIFY] [map_entry_layer.cpp](file:///home/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL/src/worldmap/map_entry_layer.cpp)
Declare and populate global maps during the one-time `prebuild_markers()` phase.

```cpp
// Global hash tables for O(1) 3D coordinate lookup
std::unordered_map<uint32_t, DiskEnemy> g_enemies_by_entity_id;
std::unordered_map<uint32_t, DiskCollectible> g_assets_by_entity_id;

// In prebuild_markers(), after loading disk_enemies and disk_collectibles:
g_enemies_by_entity_id.clear();
g_enemies_by_entity_id.reserve(disk_enemies.size());
for (const auto &en : disk_enemies) {
    if (en.entityId != 0) g_enemies_by_entity_id[en.entityId] = en;
}

g_assets_by_entity_id.clear();
g_assets_by_entity_id.reserve(disk_collectibles.size());
for (const auto &c : disk_collectibles) {
    if (c.entityId != 0) g_assets_by_entity_id[c.entityId] = c;
}
```

---

### 3. Component: Map Rendering (New Layer)

#### [NEW] [quest_npc_layer.hpp](file:///home/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL/src/worldmap/quest_npc_layer.hpp)
Definition of the marker layer for active quests.

```cpp
#pragma once
#include "marker_layer.hpp"

namespace goblin::worldmap
{
class QuestNpcLayer : public MarkerLayer
{
public:
    const char *category() const override { return "World - Quest NPC"; }
    bool visible() const override;
    const std::vector<Marker> &markers() const override;
};
}
```

#### [NEW] [quest_npc_layer.cpp](file:///home/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL/src/worldmap/quest_npc_layer.cpp)
Implementation of dynamic marker generation.
* Loop through quests.
* Find the first uncompleted step.
* If an `entity_id` is defined, look up its position in `g_enemies_by_entity_id`, then in `g_assets_by_entity_id`.
* Create a `Marker` with category `WorldQuestNPC` and icon `"show_quest_npc"`.

---

### 4. Component: User Interface (ImGui Overlay)

#### [MODIFY] [goblin_overlay.cpp](file:///home/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL/src/goblin_overlay.cpp)
* In `overlay_layers()`:
  * Exclude `Category::WorldQuestNPC` from the generic `s_cat` loop to avoid conflicts.
  * Add our dynamic `QuestNpcLayer` to the active layers list.
* In the Quest Browser (`qp_get` / `qp_set`):
  * If the step has a `progress_flag`:
    * `qp_get`: returns `goblin::ui::read_event_flag(progress_flag)`.
    * `qp_set`: calls `goblin::markers::set_event_flag(progress_flag, value ? 1 : 0)`.
  * **UX:** 
    * If a `progress_flag` is present, display the checkbox with a visual indicator (e.g., blue text or `[AUTO]` prefix).
    * Add a warning tooltip on hover explaining the impact of writing to memory.
    * If the quest has its `hostility_flag` active in memory, display an orange warning message: `[Hostile - Obtain absolution at the Church of Vows]`.

---

## Verification Plan

### Manual Tests
1. **Compilation:** Compile the DLL using CMake.
2. **Global Scope Verification:** Teleport to different zones (Limgrave, Caelid, Altus) and verify that NPCs in other zones still appear correctly on the map (proving the global index is not cleared).
3. **Hybrid Index Validation (Asset vs Enemy):** Configure a step with an `entity_id` corresponding to an Asset (e.g., Ranni's doll) and verify it displays on the map.
4. **Cheat Warnings Verification:** Open the Quest Browser, hover over an automated checkbox, ensure the warning is displayed, and test real-time modification.
