# ELDEN RING Map For Goblins — DLL

A DLL mod for Elden Ring that overlays the world map (and a minimap) with thousands of markers —
weapons, armor, spells, quest items, bosses, NPCs, merchants, graces, gathering nodes, landmarks and
more — plus its own draggable/zoomable **Virtual World Map**, marker search, click-to-warp, and a
spoiler-free mode.

**Mod-agnostic:** it works on **any** Elden Ring setup — vanilla, [ELDEN RING Reforged](https://www.nexusmods.com/eldenring/mods/541),
[The Convergence](https://www.nexusmods.com/eldenring/mods/3419), [ERTE](https://www.nexusmods.com/eldenring/mods/2747),
or your own mods — because it reads the marker data **live from the active install at runtime** (MSB
layouts, `regulation.bin` params, EMEVD, FMG text, and Oodle-decompressed game assets). Nothing is baked
per-mod: whatever the loaded game exposes is what you see, in the game's own language. Mod-specific extras
(e.g. ERR's Rune/Ember Pieces and custom map icons) light up automatically when that mod is detected and
stay disabled otherwise.

**Download:** [Nexus Mods](https://www.nexusmods.com/eldenring/mods/10062) · **Community:** [Elden Ring — DLL Mods Discord](https://discord.gg/JvTMwPCygB)

Unlike [Map for Goblins](https://www.nexusmods.com/eldenring/mods/3091), this mod does **not** modify
`regulation.bin`. All map data is injected into memory at runtime, so it won't conflict with other
regulation edits.

> **Note:** OFFLINE only. Unofficial mod, not affiliated with FromSoftware, the ERR team, the Convergence
> Team, or the ERTE author.

## Features

### Map & markers
- **Thousands of world-map markers** across 60+ toggleable categories — the exact set depends on the
  loaded game/mod (read at runtime, not a fixed bake).
- **Marker sections** — the categories are grouped into **World** (collectibles: maps, paintings, imp
  statues, kindling, interactables), **POI** (discovery landmarks: churches, ruins, forts, castles,
  dungeons, towers, evergaols…), and **Services** (graces, merchants, elevators, lifts, smithing,
  summoning pools). Each section has its own visibility toggle.
- **Virtual World Map** — a draggable, zoomable map surface with **click-to-warp** to graces, marker
  **clustering** (with spiderfy to fan out a hovered pile), and constant-size pins across zoom levels.
- **Minimap HUD** — a corner minimap in normal gameplay with the same markers, quest-NPC pins, boss
  symbols, and your death marker.
- **Marker text from the game's own files** — each marker redirects to a goods/weapon/armour/etc. name
  by ID through a MsgRepository (FMG) hook, so labels are correct and translated for free (all 14 game
  languages).

### Smart data
- **Merchant pins** are joined at runtime from the game's ESD talk scripts × MSB enemy placements —
  fully mod-agnostic, no bundled merchant table. Item search shows "sold by &lt;Merchant&gt;" attribution.
- **[Item & Enemy Randomizer](https://www.nexusmods.com/eldenring/mods/428) support** — loot markers read
  the loaded `ItemLotParam` from live memory, so each shows the item your seed actually placed (name +
  icon) and hides on the real pickup. Seed-agnostic, no per-seed data.
- **Collected-pickup detection** — collected Rune/Ember Pieces and gathering nodes are hidden
  automatically via real-time detection of the game's geometry-object state (GEOF singletons for unloaded
  tiles + CSWorldGeomMan flags for loaded tiles).
- **In-world enemy names** — the game already draws the red name tag + HP bar above enemies but names
  only bosses. MapForGoblins resolves regular (non-boss) mob names from the active install and feeds them
  into the engine's **own native tag** (`NpcParam.nameId → NpcName`), so the game renders the name itself
  in 3D world-space — correct font/accents, frame-synced with the bar, no overlay label. Per-category
  toggles (regular mobs / field-bosses / hostile NPCs) under F1 → **Enemy bars**. Mod-agnostic; on by
  default.

### UX
- **F1 overlay panel** (ImGui) — category toggles, search, settings; usable with keyboard/mouse **or
  gamepad** (stick/trigger/face-button navigation).
- **Spoiler-free mode**, two levels — **Light** (hides only randomized loot; bosses/landmarks keep names)
  and **Aggressive** (full blackout for blind runs: every marker but graces shows a colour-coded "?").
- **Item search + locate** — find a marker by name and jump the map to it.
- **Multi-language overlay UI** (`assets/lang/*.txt`).

## Installation

Grab a packaged release from the [GitHub Releases](https://github.com/quentin452/ERR-MapForGoblins-DLL/releases)
or [Nexus Mods](https://www.nexusmods.com/eldenring/mods/10062).

The mod is a single `MapForGoblins.dll`. Manual install (ERR layout):
1. Copy `MapForGoblins.dll`, the generated `MapForGoblins.ini`, and the `lang/` folder to your ERR
   `dll/offline/` directory.
2. Copy `menu/02_120_worldmap.gfx` to `addons/MapForGoblins/menu/`.

For vanilla / Convergence / ERTE, the release ships the same DLL under a `MapForGoblins/` folder for
ModEngine2 / me3 — see the release notes / Nexus page for the per-loader steps. No external data files are
needed at runtime; markers come from the live game.

## Building

The mod cross-builds to a Windows DLL with `clang-cl` (not MSVC), from **Windows or Linux**.

```bash
# Configure (once, or after CMakeLists changes)
cmake -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=clang-cl-xwin.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
# Build the single shipped DLL
ninja -C build-linux MapForGoblins
```

On Windows, `build.bat` wraps the same toolchain and can package a release; append `--vanilla`,
`--convergence`, or `--erte` to produce the other package layouts (same DLL, different worldmap GFX +
runtime-gated ERR config). See `build.bat` for the packaging targets.

> The map markers are read at runtime from the active install, so a build does **not** bake per-mod data.
> The bundled Python tools (`tools/`) handle game-data **extraction** and support assets; the embedded
> icon atlas is a transitional fallback used only until a live icon resolves (the plain circle is the
> universal fallback). See `docs/plans/baked_data_full_removal_plan.md` for the no-bake direction.

An optional **hot-reload** dev split (`goblin_overlay_render.dll`) exists behind
`-DGOBLIN_OVERLAY_HOTRELOAD=ON` for iterating on the draw layer without a restart; the default single-DLL
build is the shipped one.

## Project structure

```
MapForGoblins/
├── src/
│   ├── worldmap/          runtime marker build (build_disk_*), map render, dvdbnd/Oodle disk reader
│   ├── overlay_panel/     F1 ImGui panel (search, settings, virtual map, clustering)
│   ├── input/             cursor / raw-input / wndproc hooks
│   ├── generated_shared/  embedded fallback icon atlas
│   └── *.cpp              hooks, config, param scan, collected-state detection, warp, crash/watchdogs
├── data/                  category definitions (categories.json) + extracted support data
├── tools/                 Python extraction/build helpers + mfg.py RPC driver + rpc_tests/
├── assets/                worldmap GFX + lang/*.txt
├── docs/                  memory/ (project knowledge), plans/, re/ (reverse-engineering), changelog.md
├── CMakeLists.txt · build.bat · clang-cl-xwin.cmake
└── requirements.txt
```

## Documentation

- [Changelog](docs/changelog.md) — feature/change/fix history per release.
- `docs/memory/` — project knowledge base (features, bugs, tooling, process). Start at
  `docs/memory/common.md`.
- [Tools README](tools/README.md) — Python script + RPC-driver documentation.

## Credits

This project builds on the work of many people and projects:

### Game & Mod

- **FromSoftware** - Elden Ring
- **Elden Ring Reforged** team - the overhaul mod that inspired this project. Thanks to [**ividyon**](https://github.com/ividyon) and the ERR Discord
- **Gacsam** - [Goblin-ERR](https://github.com/Gacsam/Goblin-ERR), the original map icons mod for ERR. MapForGoblins started as a fork of this project and reuses its map fragment logic
- **Harmonixer** - [Map for Goblins](https://www.nexusmods.com/eldenring/mods/3091), the original Elden Ring map icons mod that started it all
- **Convergence Team** - [The Convergence](https://www.nexusmods.com/eldenring/mods/3419), one of the overhauls the mod supports
- **ERTE author** - [ERTE](https://www.nexusmods.com/eldenring/mods/2747), one of the overhauls the mod supports

### Libraries & Tools

- **vawser** - [Smithbox](https://github.com/vawser/Smithbox) / Andre.SoulsFormats.dll, the From Software file format library that powers all data extraction (bundled in `tools/lib/`)
- **mountlover** - [DSMSPortable](https://github.com/mountlover/DSMSPortable), used during early development for regulation and FMG editing
- **ThomasJClark** - [elden-ring-glorious-merchant](https://github.com/ThomasJClark/elden-ring-glorious-merchant/), reference for DLL mod architecture and param injection techniques
- **Dasaav-dsv** - [Pattern16](https://github.com/Dasaav-dsv/Pattern16), AOB pattern scanner; [libER](https://github.com/Dasaav-dsv/libER), Elden Ring C++ library (referenced during development)
- **vswarte** - [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs), From Software format implementations (referenced during development)
- **TsudaKageyu** - [MinHook](https://github.com/TsudaKageyu/minhook), API hooking framework
- **gabime** - [spdlog](https://github.com/gabime/spdlog), logging library
- **metayeti** - [mINI](https://github.com/metayeti/mINI), INI file parser
- **[Claude Code](https://claude.com/claude-code)** (Anthropic) - heavy lifting on the data-extraction pipeline automation and on reverse-engineering the game's in-memory geom-object state (the collected-piece detection research)

### Community

Thanks to the ERR Discord for testing and bug reports, especially **AngryPhilosopher** and **Spiswel** for early testing of the DLL version.

## License

MIT-style, see [LICENSE.txt](LICENSE.txt) — includes the original [Goblin-ERR](https://github.com/Gacsam/Goblin-ERR) notice (this project started as its fork) and the bundled third-party licenses (Pattern16, MinHook, HDE64, mINI, spdlog).
