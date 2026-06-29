---
name: thread3-grace-toggle
description: "Thread 3 (per-family icon toggle) — ERR rewrote the grace menu, so go DLL-native (path B), not grace-ESD"
metadata: 
  node_type: memory
  type: project
---

Thread 3 = per-family map-icon toggle in-game. Original Map for Goblins (Nexus 3091, Harmonixer) did it param-only: edited `m00_00_00_00.talkesdbnd` (grace talk ESD) + `BonfireWarpParam` to add grace-menu options that flip event flags; `WorldMapPointParam.eventFlagId` gates each family.

**ERR replaced the grace menu.** ERR ships its own `mod/script/talk/m00_00_00_00.talkesdbnd.dcx` (524 KB) — "Reforged site of grace settings menu" (CHANGELOG:2168) plus transmog/boss-rush/perfect-attack toggles. Original mod's grace-ESD edit collides head-on → that's why MapForGoblins is a DLL, not a param mod.

**Why:** editing the grace ESD to inject MFG options would clobber ERR's overhauled menu.

**How to apply:** Thread 3 = path **B** (user-chosen 2026-06-18). NOT grace-ESD (path A), NOT per-family eventFlagId (path C, blocked on flag-writer RE — see [[mapforgoblins-map-open-freeze]]).

**SHIPPED (MVP, 2026-06-18, not yet committed; built+deployed to ERR install dll/offline/, old DLL = .bak-prethread3):** per-**section** (7 groups: Equipment/Key Items/Loot/Magic/Quest/Reforged/World) in-game toggle. Granularity=per-section, UX=hotkey cycle+toast, persist=INI (all user-chosen).
- Mechanism: NO param rebuild. Blob always holds all family-enabled rows; section gate flips each injected row's `areaNo` → 99 (hide) / restore (show) in place on the live expanded blob (same eviction trick as collected pieces). `section_of(Category)` + `g_section_visible[7]` + `g_section_rows` registry + `apply_section_visibility()` in `goblin_inject.cpp`.
- UX: F8 cycles selected section, F7 toggles it (config `section_select_key`/`section_toggle_key`/`enable_section_toggle`). Hotkey thread posts intent → watcher `menu_auto_toggle_loop` applies+persists+toasts (single game-state owner). 14 toast strings (7×shown/hidden) at TUTORIAL_FMG_ID_SECTION_BASE=9004260.
- Config: 7 `section_*` bools (default true=no regression) in new `[Display Sections]` INI block; `save_section_states()` writes back via mINI `write()` (preserves file). ini auto-migrates on load.
- Family-level (~60) stays INI-only. Builds clean via build-linux ninja. Committed on branch `feat/section-toggle` (2cbcddd, off feat/quest-npc-layer HEAD — depends on Thread1's Category::WorldQuestNPC enum, so can't branch from master). PENDING: in-game verify on Windows.

**Live-refresh (toggle while map OPEN) — RE groundwork, docs/world_map_live_refresh_re.md:** game builds icon list once per map-open, never re-reads param while open → flip only shows on reopen. Subsystem (from eldenring.exe cleartext RTTI): `CSWorldMapPointManImplement` (FD4Singleton) builds `CSWorldMapPointIns` from WorldMapPointParam; `_DiscoverMapPoint`; `WorldMapPointParam::_VerifyEnable/DisableEventFlag` (per-row gate). Anchor string VAs: _DiscoverMapPoint=0x142b48c1c, _VerifyEnableEventFlag=0x142bb5d84, _VerifyDisableEventFlag=0x142bb5db4. Pin the rebuild method by AOB (LEA-xref trick like ShowTutorialPopup), call from menu_auto_toggle_loop after flip when CSMenuMan+0xCD==7. BLOCKED on Linux box: packed exe + no decompiler/debugger (objdump linear sweep misses xrefs); needs Ghidra/IDA + running game on Windows.
