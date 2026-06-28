---
name: runtime-icon-coverage
description: "Runtime per-item icon coverage — what's solved/shipped, the proven runtime ceiling, and the one untried lever (resume point for the icon work)"
metadata: 
  node_type: memory
  type: project
---

Resume point for the ERR map-icon work (session 2026-06-22, branch `docs/worldmap-projection-re-brief`).
Full RE in `docs/re/windows_resident_icon_enumeration_re_findings.md` (§1 repo, §5b CSFile load, §5c gfx
binding, §5d residency manager); session TL;DR in `docs/re/windows_runtime_icon_session_brief.md`.

**SHIPPED + working (committed):** resident-icon enumeration via the repo `std::map` at
`*(er+0x3d82510)+0x80` (`harvest_repo_icons()`); format via `ID3D12Resource::GetDesc()` at copy time
(not RPM pre-bind); BC 4-block snap + UV crop (fixed 270×270 cells); repo-walk re-walks on ANY `_Mysize`
change (game oscillates/evicts) so browse-to-fill accumulates the union into our PERMANENT write-only
cache `g_harvest`. CE `.CT` + `<ghidra_scripts>\walk_icon_repo.py` / `walk_both_maps.py` /
`monitor_icon_load.py` / `monitor_icon_union.py` / `force_load_file()` test harness.

**PROVEN runtime ceiling (don't re-investigate):** no "global page" — the game STREAMS (~150 item-icon
window; `_Mysize` churns; repo+0x80 is the right map, twin repo+0xb0 has 0 icons; session-union grew
151→216/min browsing). Force-load (CSFile `er+0x1f5560`, callable, confirmed) loads the TPF atlas but
NOT the per-icon rects — rects come from the sblytbnd via the gfx BINDING which is VIRTUAL/menu-applied
(vtables 0x493c8f0/0x378bf40, not standalone-callable). Residency is governed by the menu resource
manager (queues +0x1df0 load / +0x1e08 unload, flag +0x1e19, update FUN_140d78540→FUN_140d70940,
tickers FUN_140d724c0/d78060), reached via the task system (not a flat global). The baked category
atlas (`goblin_overlay_icons` + stb_image) is NOT deletable — it's the always-available base for
markers + the F1 config UI; GPU harvest is an override on top. Marker `.iconId` (370-443) =
WorldMapPointParam pin frames, a DIFFERENT namespace from MENU_ItemIcon item icons.

**Why:** <user> wants real per-item icons on map loot markers; established it's only cleanly possible
for BROWSED items at runtime (own inventory + visited shops), not the whole game.

**Decision (2026-06-22): drop eviction-blocking** (ImGui doesn't evict, `harvested:` is stable). Goal
reframed = **force a non-resident icon RESIDENT on demand**. Asked: is the lever in `FUN_140d724c0`?
**Answered NO** — decompiled the whole tick chain (re_v119, §5e in the findings doc; scripts
`find_resmgr.java`/`find_resload.java`, `<ghidra_scripts>\out_resmgr.txt`/`out_resload.txt`):
- `FUN_140d724c0` is only the PUMP (drains queues if `mgr+0x1e19` dirty, else per-tick apply). `mgr=*param_2`.
- FILE-load half = cleanly callable BY groupId: `FUN_140d77550(mgr,gid)`=TPF, `FUN_140d771d0(mgr,gid)`=
  sblytbnd(rects), guarded by handle `mgr+0xd10/0xd58+gid*8`. Wrappers `FUN_140d6ae40/60/80/aef0/b000`
  (sblytbnd set = gid {1,2,3,4,5,6,8} via `FUN_140d6aef0`). **These ONLY load files — NO bind** (§5c wall confirmed).
- ★ The BIND has a tick trigger = a **FLAG FLIP** (not a fabricated +0x1df0 request): `FUN_140d78540`
  loops groups `mgr+0x9d8` (count `mgr+0xbe0`, stride 0x10); for any `entry+0x7c!=0` → `FUN_140d70940`
  → vmethod `(entry+0x18)->+0xc8` (apply) → `entry+0x80=1`. Much simpler than §5d's request-synthesis.

**SHIPPED the test (2026-06-22, commit on branch):** `goblin::bind_test(action,gid)` + P2b buttons,
gated `config::dumpIconTextures`. Hooks ticker `FUN_140d724c0`, captures mgr=`*param_2`, runs inline
before orig: 1=dump groups (logs `entry+0x18` res + `apply(vt+0xc8)` RVA + flags `+0x7c`/`+0x80`),
2=load files gid via `FUN_140d77550`+`FUN_140d771d0`, 3=flip-bind all (`+0x7c=1`+dirty), 4=load+flip.
**★ RUNTIME RESULT (<user> tested 2026-06-22): +0x7c flag-flip REFUTED.** Manager captured, 15 group
entries, but `harvested` stayed 162→162 on ALL actions (flip/load/dump/load+flip) — NO repo growth.
Every group's apply `vt+0xc8` = SAME fn `FUN_14112fc80` = a **Scaleform display-tree RENDER method**
(Matrix3x4 MultiplyMatrix over the movie tree), NOT a sblytbnd parse. The manager's `+0x9d8` "groups"
are the 15 loaded MOVIES; flipping +0x7c just re-renders them. So runtime force-residency is EXHAUSTED:
force-load=bytes-no-rects, +0x7c=render-no-rects, +0x1df0=find-and-activate movies + keep-alive timer
(req+0x4c float countdown), find-only so can't introduce new rects (re_v121, find_loadq.java —
<user>'s "does the load queue bind itself?" also refuted). Per-icon rect-bind is ONLY
the menu-init virtual apply on `ShoeboxLayoutbndFileCap` (§5c, not callable). **DECISION = OFFLINE BAKE
is the path** (extend `tools/extract_subtextures.py` → emit iconId→(atlas,rect); repo-walk stays as
freshness override). Empirically confirmed, not theoretical. `bind_test` harness kept as gated diagnostic.

**★ RUNTIME CE "find what writes to _Mysize" (re_v123, §5g, <user> tested 2026-06-22) — closes it
live.** Pinned repo+0x90, captured both writers + stacks (session er=0x7FF61AB80000): INSERT `_Mysize++`
@RVA 0xD611AB, ERASE `_Mysize--` @RVA 0xD6746E. Stacks asymmetric = the proof. INSERT stack: FUN_140d61180
←d61ff0←d64f30←★FUN_140d66a60←[VMP @d6648b/@d63dbd] = driven by the VMProtect provider-apply parse (the
widget FUN_14074bcc0 frames were STALE — decompile shows it only calls FUN_140d63e50 read-only find, no
insert edge). ERASE stack: CLEAN, all defined — ticker FUN_140d78060→orchestrator FUN_140d790a0(+b5a,
unload pass)→FUN_140d70740/180→Scaleform release→FUN_140d68780/68690→erase. So repo FOLLOWS the movie
lifecycle: insert=movie BIND (VMP, untraceable trigger), erase=manager unload (defined). No on-demand
per-icon load trigger exists — confirmed live from both ends. OFFLINE BAKE remains the path.

**Last two levers closed (re_v122, §5f):** E=CreateImage (`FUN_140d6bbc0`→`FUN_140d64490`) builds only
`<symbol>_ptl` name+DLString, NO rect read → name-view needing an already-bound sheet = dead. D=parse:
rect-leaves `FUN_140d650b0`→`FUN_140d68410` (writes CSTextureImage vtable+rect img+0x74..) are concrete
& understood, but the loop feeding them valid sprite-ENTRY objects is VMP-indefinite (region 0xd663-0xd667,
provider vtables 0x493c8f0/0x378bf40, apply-time only) — no bounded entry; calling the leaf by hand =
fabricate a layout-entry per icon = re-implement the sblytbnd parse = exactly what `extract_subtextures.py`
does offline. So the runtime layer to drive IS the offline layer. ALL evident levers exhausted → OFFLINE BAKE.

**Earlier framing (gated RUNTIME experiment, [[rpm-live-memory-tooling]]):** pin mgr live
(hook ticker `FUN_140d724c0` grab `*param_2`, or getter `FUN_140d69c20`/`FUN_140d69b40`); (a)
`FUN_140d77550(mgr,1)`+`FUN_140d771d0(mgr,1)` (gid 1=01_Common); (b) walk `mgr+0x9d8`, find 01_Common
entry, LOG `entry+0x18` vtable + `+0xc8` target; (c) set `entry+0x7c=1`, `mgr+0x1e19=1`, wait ticks;
(d) re-harvest repo — did distinct `MENU_ItemIcon` jump for un-browsed items? Gate like `force_load_file`,
VRAM-watch, one group at a time. Two unknowns to confirm: 01_Common is a registered `+0x9d8` entry
un-browsed, AND its `+0xc8` apply is the sblytbnd parse (`FUN_140d650b0/d66520` §5c), not a generic
mark-resident. If repo gains rects → runtime 100% lever; else **offline bake stays the path** (extend
`tools/extract_subtextures.py` to emit iconId→(atlas,rect), keep repo-walk as freshness override).
Fallback always-available: browse-to-fill + 63 category pins. See [[ghidra-worldmap-re]], [[workflow-preferences]].

**★ GRACE ICON CORRECTION (2026-06-22, re_v125, §5i):** `SB_ERR_Grace_*_Color` = WRONG (native
time-tinted pin: morning green / evening red, no untinted variant — NOT a format bug, item icons render
fine via identical code). The REAL grace = WorldMapPoint pin: `WorldMapPointPinData::GetIconId`
(FUN_14087bf20, grace=iconId 370) → `WorldMapPinData::SetTo` (FUN_14087ae20) gets Scaleform "Icon_0"/
"IconImage" → widget FUN_14074bcc0 builds `"Frame%d"` (Frame370) → find via FUN_140d63e50 in the **TWIN
map repo+0xb0** (head +0xb8, size +0xc0), keyed by gfx sprite name **`MENU_MAP_GOBLIN_Grace`** (in
addons/MapForGoblins/menu/02_120_worldmap.gfx; siblings MENU_MAP_ERR_Boss/Camp/Completed/GraceUnderground
/Remembrance, MENU_MAP_GOBLIN_SortaGraceIDK). We only ever walked repo+0x80 (MENU_ItemIcon) → never had
the real grace. SHIPPED: `harvest_twin_map_icons` walks repo+0xb0, captures MENU_MAP_* into grace
candidates (F1 debug picker), locks grace on MENU_MAP_GOBLIN_Grace. F1 "Grace texture debug (live)" panel
= format/swizzle/source picker. RUNTIME-UNVERIFIED (<user> tests the twin-walk capture).
