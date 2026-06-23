# Task: generate the non-ERR profile data for MapForGoblins (Windows)

> **Status (2026-06-17 — ALL profiles generated & delivered):**
> - ✅ **vanilla** — generated, re-gen'd, delivered. `MAP_ENTRY_COUNT = 6812`, DLC
>   present (area 60 = 2597, area 61 = 826). The category-selective DLC gap below is
>   **fixed** — see *FOLLOW-UP — RESOLVED*.
> - ✅ **convergence** — generated, delivered. `MAP_ENTRY_COUNT = 7373`, DLC present
>   (area 60 = 2960, area 61 = 799). Icon-frame offset +408 applied automatically.
> - ✅ **erte** — generated, delivered. `MAP_ENTRY_COUNT = 7720`, DLC present
>   (area 60 = 2897, area 61 = 870).
>
> **Deliverables:** `generated_vanilla.zip`, `generated_convergence.zip`,
> `generated_erte.zip` at the repo root — each = the full `src/generated_<p>/` dir plus
> its `<p>_generate.log`. Coverage tables regenerated with all four columns
> (vanilla, ERR, conv, erte): `coverage_base.md` / `coverage_dlc.md` /
> `coverage_summary.md`. Prereqs set up on this machine: game UXM-unpacked (with DLC),
> `tools/config.ini` filled (game_dir + convergence_mod_dir + erte_mod_dir), Python deps,
> `oo2core_6_win64.dll` placed for SoulsFormats.
>
> **FOLLOW-UP — RESOLVED.** The vanilla DLC zeros (Cookbook / Map Fragment / Bell
> Bearing / Crystal Tear / Great Rune) were two real generation bugs, both fixed and
> merged to `master` (PR #1):
> 1. `extract_all_items.read_fmg_names` skipped the DLC name FMGs (`GoodsName_dlc01.fmg`),
>    so DLC-exclusive items got empty names and the *name-based* loot categories dropped
>    them. (ERR re-bakes names into the base FMG → only non-ERR profiles were hit.)
> 2. The Rune Arc filter hard-coded ERR's goods id 150; in vanilla id 150 is *Furlcalling
>    Finger Remedy* and Rune Arc is id 190 → vanilla mislabeled/dropped them. Now
>    profile-gated.
>
> vanilla now matches MapGenie/ERR (Map Fragment exact; Bell Bearing/Crystal Tear/Great
> Rune = ERR; Cookbook finds all 45 placements → 37 markers after flag-dedup; base Rune
> Arc 58 ≈ MG 63). The **Convergence** DLC zeros (Scadutree / Cookbook / Crystal Tear) are
> NOT bugs — the overhaul keeps those names at vanilla ids but doesn't place them as
> collectible world lots (details in `coverage_summary.md`). Original brief preserved
> below for reference.

---

## FOLLOW-UP: re-generate vanilla — category-selective DLC zeros

**What's wrong.** In the delivered `src/generated_vanilla/`, these categories read **0
on the DLC map (area 61)** even though they exist in the vanilla DLC and ERR finds them:

| Category | generator | MapGenie (DLC) | vanilla (DLC) | ERR (DLC) |
|---|---|--:|--:|--:|
| Cookbook | `generate_loot_massedit.py` | 45 | **0** | 41 |
| Map Fragment | `generate_maps.py` | 5 | **0** | 5 |
| Great Rune | `generate_loot_massedit.py` | 1 | **0** | 0 |
| Bell Bearing | `generate_loot_massedit.py` | 10 | **0** | 7 |
| Crystal Tear | `generate_loot_massedit.py` | 8 | **0** | 5 |

This is **not** a whole-DLC-tier failure (779 area-61 rows did come through). It is
**category-selective**, which points at a per-category generation stage that didn't read
the DLC (area 61) item lots for these categories during the vanilla run — a real
generation bug, not a content/filter difference. (A clean vanilla+DLC run cannot
legitimately yield 0 cookbooks when 45 exist and Scadutree did come through.)

**Possibly also affected (base map, unconfirmed):** vanilla also reads far below ERR for
**Rune Arc** (vanilla 19 / ERR 66 / MG 63) and **Spirit Ashes** (vanilla 12 / ERR 56 /
MG 65) on the *base* map. This may be legitimate (those items come from enemy
drops/shops that the loot pipeline filters by design, and ERR/Reforged places them as
flagged pickups so only ERR passes the filter) — **or** the same generation gap. Worth
confirming while you're in there.

**Re-run steps (Windows, repo root):**
1. Re-run the vanilla data pipeline and **capture the full console log**:
   ```bat
   build.bat --vanilla generate > vanilla_generate.log 2>&1
   ```
2. In `vanilla_generate.log`, look for the per-category stages — `generate_loot_massedit`
   (cookbooks, bell bearings, crystal tears, rune arcs, spirit ashes) and `generate_maps`
   (map fragments). Check for: errors/exceptions, a missing/empty DLC item source, a
   skipped DLC (area 61) pass, or a `config.PROFILE`/path branch that diverges from the
   `err` run. Note which stage drops the area-61 rows for these categories.
3. Verify the regenerated `src/generated_vanilla/goblin_map_data.cpp`: it must now
   contain area-61 (`.areaNo = 61`) rows for the cookbook / map-fragment / bell-bearing /
   crystal-tear categories. Confirm the total area-61 row count rises above the current
   779.
4. **Deliver:** the regenerated `src/generated_vanilla/` (zip, same as before — do NOT
   push a branch, nothing is published) **plus `vanilla_generate.log`** so the cause can
   be confirmed. If the stage genuinely finds no DLC entries for a category (i.e. it's
   correct and MapGenie counts a source we filter), say so with the log evidence instead
   of forcing rows in.

> Note: only the final `goblin_*.cpp` files were returned last time, so the vanilla
> intermediate MASSEDIT files are not on the Linux box — the cause can only be diagnosed
> from a fresh run's log on the Windows machine. That's why the log is a required
> deliverable this round.

---


You are working on the **ERR-MapForGoblins-DLL** repo (an Elden Ring world-map icon
mod). It builds in **4 profiles** — `err`, `vanilla`, `convergence`, `erte` — each
driven by its own *generated* data under `src/generated_<profile>/`. Only the `err`
profile data (`src/generated/`) is committed; the others are git-ignored and must be
regenerated by the data pipeline.

We need those generated profiles (especially **vanilla**) on another machine to run a
coverage comparison (mod counts per profile vs MapGenie). Your job is to **run the
data pipeline for the missing profiles and return their generated dirs.**

## Deliverables (in priority order)

1. **`src/generated_vanilla/`** — the whole directory. (highest priority — vanilla =
   same content as MapGenie, so it's the clean baseline.)
2. `src/generated_convergence/` — the whole directory.
3. `src/generated_erte/` — the whole directory.

For each, the two files the comparison strictly needs are **`goblin_map_data.cpp`**
and **`goblin_legacy_conv.hpp`**, but please return the entire `src/generated_<p>/`
dir so nothing is missing.

**Delivery:** either push them on a branch (e.g. `profiles-data`, temporarily
un-ignoring the dirs or `git add -f`), or zip the three dirs and hand the zip back.

## Prerequisites to set up

- This repo cloned, on Windows.
- **Visual Studio 2022** with the C++ desktop workload (build.bat probes it via vswhere).
- **Python 3** + `pip install -r requirements.txt` (pythonnet, pymem, psutil).
- `tools/lib/Andre.SoulsFormats.dll` is already bundled. `oo2core_6_win64.dll` is taken
  from the game dir.
- **`tools/config.ini`** with the paths the pipeline reads (create it; see
  `tools/config.py` for the keys). Minimum:
  ```ini
  [paths]
  game_dir = C:\path\to\UXM-unpacked\ELDEN RING\Game   ; UXM-unpacked vanilla game (needed for vanilla)
  err_mod_dir = C:\path\to\ERR\mod                      ; ERR mod overlay (already have if err builds)
  convergence_mod_dir = C:\path\to\TheConvergence\mod   ; only for convergence
  erte_mod_dir = C:\path\to\ERTE\mod                    ; only for erte
  smithbox_dir = C:\path\to\Smithbox                    ; if a stage needs it
  ```
  - **Vanilla needs the game UXM-unpacked** (loose files), because the pipeline reads
    `game_dir` directly. Use UXM Selective Unpacker on a vanilla ELDEN RING install.
  - **The game install MUST include the Shadow of the Erdtree DLC.** One `generate`
    per profile extracts BOTH the base map (area 60) and the DLC map (area 61) into a
    single `goblin_map_data.cpp` — there is no separate base/DLC run — but if the DLC
    files aren't present/unpacked, the area-61 (DLC) rows will simply be missing.
  - Convergence / ERTE need those overhauls' `mod` overlay dirs installed.

## Commands

Run the **data pipeline only** (no DLL build) per profile, from the repo root:

```bat
build.bat --vanilla generate
build.bat --convergence generate
build.bat --erte generate
```

(`build.bat generate` alone = the `err` profile, which we already have.) Each writes
its `src/generated_<profile>/` dir. The pipeline is hash-cached, so re-runs are cheap.

## Validation before delivering

Each `src/generated_<p>/goblin_map_data.cpp` starts with `const size_t
MAP_ENTRY_COUNT = N;`. Sanity-check N against the README's advertised icon counts:

- vanilla ≈ **6700**
- convergence ≈ **7200**
- erte ≈ **7600**
- (err, for reference, ≈ 8952)

Also confirm the **DLC came through**: each file must contain both `\.areaNo = 60`
and `\.areaNo = 61` rows (the err file has ~7043 area-60 and ~1781 area-61). If
`.areaNo = 61` is absent, the DLC wasn't unpacked — fix the game install and re-run.

If a profile errors out (missing path, missing mod overlay, UXM not unpacked), report
which prerequisite failed rather than delivering a partial/empty dir. Vanilla is the
one that matters most — if convergence/erte can't be set up, still deliver vanilla.

---

## FOLLOW-UP (2026-06-17): generate the Quest-NPC layer

New feature (Thread 1): a **named friendly NPC + merchant** location layer for quest
navigation. Code is wired on the Linux side (DLL enum `WorldQuestNPC` + config
`show_quest_npc` + `CATEGORY_MAP` + a `build_pipeline.py` Stage + `PRESERVE_FILES`).
The generator `tools/generate_quest_npcs.py` exists but its filter must be TUNED with
real data — it cannot run on Linux (needs pythonnet/SoulsFormats + the ERR mod).

Steps (Windows, ERR profile):

1. **Tune the friendly teamType set.** Run:
   ```
   python tools/generate_quest_npcs.py --inspect
   ```
   It prints `teamType -> placed named-NPC count` with samples (npc/nameId/model/part).
   Identify which teamTypes are friendly NPCs/merchants (Kalé, Twin Maiden Husks,
   Roderika, Gostoc, Boc, …) vs enemies/bosses. Set `FRIENDLY_TEAM_TYPES` near the top
   of the script accordingly (current default `{1,2,6,7,8}` is a GUESS — replace it).
   Exclude any teamType whose samples are enemies/bosses (bosses are a separate layer).

2. **Pick the icon.** `ICON_ID` defaults to 374 (same as hostile NPC = safe-renders
   placeholder). Choose a distinct friendly/quest worldmap icon and confirm it renders
   in-game; update the constant. (User requirement: this family is visually distinct and
   must NOT be clustered when clustering ships.)

3. **Generate + sanity check.** Run `build.bat generate` (err profile) — the new Stage
   emits `data/massedit_generated/World - Quest NPC.MASSEDIT` and re-bakes
   `src/generated/goblin_map_data.cpp` (now including `Category::WorldQuestNPC` rows).
   Confirm a sane marker count (expect a few hundred — named NPCs are not numerous) and
   that the names resolve (textId1 = nameId + 700000000). Spot-check a known NPC's
   position on the map.

4. **Deliver** the regenerated `src/generated/goblin_map_data.cpp` (+ the new MASSEDIT)
   and the final `FRIENDLY_TEAM_TYPES` / `ICON_ID` values used, so the Linux side can
   commit the tuned generator. If counts look noisy (enemies leaking in), report the
   teamType breakdown so we narrow the set.
