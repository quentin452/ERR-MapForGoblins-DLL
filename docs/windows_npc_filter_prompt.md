# Task: fix the friendly/quest-NPC marker filter + regenerate (Windows, ERR profile)

You are on a **Windows** box with **ERR-MapForGoblins-DLL** cloned and the **err**
profile building (VS2022 + Python + `tools/config.ini` valid; ERR `regulation.bin`
+ `map/MapStudio/*.msb.dcx` present). The data pipeline runs here only.

## Background

`tools/generate_quest_npcs.py` builds the `WorldQuestNPC` layer (named friendly
NPCs + merchants). Filter today: `teamType ∈ {0,1,2,26}`, `EXCLUDE_MODELS={c1000}`,
`nameId > 0`, placed `EntityID > 0`; row-id base 9400000; marker label
`textId1 = nameId + 700000000`. A coverage audit against the game's own NpcName FMG
(`data/npc_name_text_map.json`) found three problems to fix. **All nameIds below
are confirmed in that map; verify each against live NpcParam/MSB before trusting.**

## Problems to fix

### A. Missing resident merchants (filter too narrow)
These EXIST in-game and are core vendors but get NO marker (their team is outside
`{0,1,2,26}` — they're Roundtable/static "resident" NPCs, not the questline teams):
- **Twin Maiden Husks (160000)** — THE Roundtable Hold bell-bearing merchant. The
  single most important omission. Run `python tools\generate_quest_npcs.py --inspect`
  and find which teamType holds nameId 160000.
- **Preceptor Miriam (135200)** — sorcery merchant, Shaded Castle.
- **Asimi, Silver Tear (121800)** / **Asimi, Eternal King (121810)** — Carian Manor.
- DLC: **Ancient Dragon Florissax (140601)** (Dragon Communion questline).

### B. False positives (objects/enemies tagged as NPCs — remove)
Leaking through the friendly teamTypes; they are NOT navigable NPCs:
- **Altar of Anticipation (170000)** — 8 markers of a map/event object (worst).
- **Church of Dragon Communion (160100)**, **Cathedral of Dragon Communion (160500)**,
  **Smithing Table (160200)** — location/altar labels.
- **Lord's Journey (121608)** + the **121601–121610** menu family (a c1000-family menu
  object placed under a non-c1000 model, so `EXCLUDE_MODELS` missed it).
- **Equilibrious Rat (904080600)** — a DLC enemy that leaks via the generator-name
  (1.6-billion `textId1`) encoding.
- Borderline (decide): Rennala (120000), Torrent (110100) — a boss + the mount.

### C. Data anomaly
- **nameId 133200** — 4 markers but NO entry in the NpcName FMG → renders blank.
  Investigate the NpcParam behind those placements; likely an unnamed enemy with a
  stale nameId. Drop or correct.

## Recommended approach

Prefer an **allowlist** over widening teamType blindly:
- Keep `WorldQuestNPC` to the NpcName **character ID range** (~110000–144999 and the
  merchant range ~175000–180999), MINUS an explicit **object/menu denylist**
  (170000, 160100, 160200, 160500, 121601–121610, plus the `9xxxxxx` generator-name
  enemies like 904080600).
- For the static merchants in (A), add their team once `--inspect` shows it's
  friendly-dominated, OR special-case the known merchant nameIds (160000, 135200,
  121800/121810, 140601).
- Keep the row-id base 9400000 (do NOT reuse 9300000 — it collides with hero-tomb
  statues; generate_data is last-writer-wins and silently clobbers).

## Steps
1. `python tools\generate_quest_npcs.py --inspect` — capture the teamType→sample dump
   (needed to place Twin Maiden Husks + audit teams). Include it in the deliverable.
2. Edit the filter per above (allowlist range + denylist + merchant special-cases).
3. `build.bat generate` (err profile). Sanity-check: count is sane (a few hundred);
   Twin Maiden Husks now present (`grep "= 700160000," src\generated\goblin_map_data.cpp`);
   the false-positive nameIds (170000/160100/…) GONE; 133200 resolved.
4. Spot-check a few placements land on the right map area (Twin Maidens @ Roundtable
   appears via the Roundtable's overworld projection if applicable).

## Deliver
1. Regenerated **`src/generated/goblin_map_data.cpp`** (+ `data/massedit_generated/World - Quest NPC.MASSEDIT`).
2. The final filter constants (teamTypes / allowlist range / denylist / merchant special-cases).
3. The full `--inspect` dump (evidence; lets the Linux side narrow further).
4. Before/after marker counts + the list of nameIds added/removed.

Don't push a branch; `src/generated/` is committed, but the tuned constants land via
the Linux box. Zip / paste back the files + values + inspect dump. If a teamType is
ambiguous (mixed friendly + hostile samples), report its samples rather than guessing.
