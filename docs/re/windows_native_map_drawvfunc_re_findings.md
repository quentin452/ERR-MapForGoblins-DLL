# Native world-map DRAW vfunc — RE findings (candidate 1 result: no per-menu draw vfunc; the map is
# submitted centrally by CSScaleform)

Answers `windows_native_map_drawvfunc_re_prompt.md` (candidate 1 of
`windows_native_map_render_toggle_re_findings.md`). Static Ghidra **headless** on `D:\ghidra_proj2` /
`eldenring.exe` (imagebase 0x140000000, App 2.6.2.0 / ERR 2.2.9.6), 2026-07-06. `er+` = RVA off 0x140000000.

**Status: candidate 1 (goal A — a per-menu DRAW vfunc slot on WorldMapDialog/MenuWindow) is DISPROVEN with
full evidence. The native map draws through a CENTRAL CSScaleform pass, not a menu draw vfunc.** The CSScaleform
module is now mapped end-to-end (singleton, System movie-manager, per-frame Step, RTTI vtables, player struct)
so goal B (a render-enable/visible lever) has a narrow target — but the final DISPLAY-submit offset is not yet
pinned (it runs on the render thread; see §4 for the exact continuation).

---

## 0. TL;DR
- **Goal A is dead.** `CS::WorldMapDialog`'s vtable (er+0x2b2d7d8) has **exactly 13 slots** (fully
  enumerated, §2). **None** of them touch the movie handle (`dialog+0x140`) or submit a GFx movie — they are
  the CSMenu/MenuWindow lifecycle + list-builder vfuncs. And `CSMenuManImp`'s ctor registers **only ONE**
  per-frame task (`CSEzUpdateTask` → `FUN_140766980`, the UPDATE/logic tick) — there is **no second "draw"
  task** that walks menus calling a draw vfunc. So "no-op the WorldMapDialog draw slot" has **no slot to
  no-op**. This confirms §4 of the render-toggle findings from the disassembly side.
- **How the map actually draws:** WorldMapDialog builds a GFx movie (`02_120_worldmap.gfx`); that movie is
  registered with the global **`CS::CSScaleform`** singleton and rasterized by a **central CSScaleform pass**
  during present/render — independent of the menu vtable. Hiding the map = stopping THAT movie from being
  submitted, while its ADVANCE (animation/logic) keeps running.
- **Goal B (the real path) — the lever is a per-movie "render/visible" gate, not a menu slot.** The remaining
  work is to pin the field/method the DISPLAY-submit consults. §3 maps every anchor needed; §4 gives the two
  ways to finish it (Ghidra render-thread trace, or the faster Linux-live RPM probe of the now-narrow
  0xe8-byte player struct — the B3 lesson: live beats static here).

## 1. Method
Ghidra 12.1.2 headless, `-process eldenring.exe -noanalysis -postScript` against the pre-analyzed
`D:\ghidra_proj2` project (no re-import). Scripts in `D:\ghidra_scripts\mfg_*.java` (drawvfunc / drawvfunc2 /
scaleform / scaleform2 / scaleform3 / sfvt / sfdraw / sfadv / sfsys / sfvis). RTTI was resolved by raw byte
search (the DB has no reference edges for MSVC RVA-based COLs; see §6). The game's `CS::CSScaleform*` wrappers
are **not** symbolized in this DB — only the vanilla `Scaleform::` middleware is — so everything CS-side is
`FUN_`.

## 2. Goal A DISPROVEN — WorldMapDialog has no draw vfunc; the manager has no draw task

**WorldMapDialog vtable @ er+0x2b2d7d8 = 13 slots** (slot 13 is the adjacent `02_120_worldmap` UTF-16 string,
i.e. the vtable ends at 13). Single-inheritance: `FUN_140741960` (CSMenu base ctor) stamps
`DLReferenceCountObject → CS::SceneObjModifier → CS::MenuWindow` all at `*self` (one vtable at offset 0).
Slots (⟶ = decompiled role; `9cxxxx` = WorldMapDialog override, `74xxxx` = inherited CSMenu/MenuWindow):

| slot | vt+off | fn | role |
|----|------|------|------|
| 0  | +0x00 | FUN_1407342b0 | base dtor-ish (Unref helper) |
| 1  | +0x08 | FUN_1409cf8f0 | **deleting dtor** (re-stamps vftable, frees 0x3ed0) |
| 2  | +0x10 | FUN_1409cfb60 | WMD override — reads +0x30e8, writes +0x3ecc (a getter/refresh) |
| 3  | +0x18 | FUN_1407451b0 | trivial getter (`*out = 0`) |
| 4  | +0x20 | FUN_1409c1e70 | WMD override — calls vfunc +0x50, list work (+0x5b6/+0x5e1) |
| 5  | +0x28 | FUN_140744790 | `return vfunc[+0x50]() == 0` predicate |
| 6  | +0x30 | FUN_140744f30 | inherited — iterates list +0x1f8..+0x200, vfunc +0x10 |
| 7  | +0x38 | FUN_140745bd0 | inherited — builds 0x88-stride array, vfunc +0x20/+0x48 |
| 8  | +0x40 | FUN_140745b70 | inherited — small setter into +0x2f8 |
| 9  | +0x48 | FUN_140745a30 | inherited — 0x88-stride array builder |
| 10 | +0x50 | FUN_1409c2970 | WMD override |
| 11 | +0x58 | FUN_1407459d0 | inherited |
| 12 | +0x60 | FUN_140746e80 | inherited |

**None reference `dialog+0x140` (the movie handle) or call into CSScaleform.** These are open/close/list/cursor
lifecycle + the list-builder helpers, not a movie submit. There is no draw vfunc here.

**Manager side — only one task:** `CSMenuManImp` ctor `FUN_1407650a0` registers a **single**
`CS::CSEzUpdateTask<CS::CSEzTask,CS::CSMenuManImp>` (vftable stored at `this[0x10d]`), whose execute is
`FUN_140766980`. `FUN_140766980` is the UPDATE tick (dev-hotkey polls `FUN_140ddb560()==0x2d/0x38/0xa`, timers,
sub-steps `FUN_140767180`/`FUN_1407668f0`/`FUN_140767180`). **No second (draw) task is registered.** So the
menu system does not iterate menus calling a draw vfunc during render — the draw lives in CSScaleform (§3).

## 3. The real render path — CSScaleform module map (all live-re-resolvable anchors)

The world-map movie is drawn by the global **`CS::CSScaleform`** singleton, not the menu. Mapped:

```
CSScaleform singleton (CSScaleformImp*)   image slot  er+0x3d83148  (FD4Singleton; asserts on null)
  CSScaleformImp  RTTI vtable              er+0x2bb92f8   (thin: [0]=getInstance FUN_140d6b4b0, [1]=dtor FUN_140d6a740)
  CSScaleformSystem  (movie manager)       = *(CSScaleformImp + 0x08)
      movie list / RB-tree of movies       System + 0x9b0   (walked by FUN_140d6b050)
      reload flag (ReleaseAllTextureForReload) System + 0x1e19  (set by FUN_140d79d10)
CSScaleform per-frame STEP execute         FUN_140d6e6f0 (er+0xd6e6f0)  — RTTI "StepTask< CSScaleformStep >";
                                             takes (self, FD4Time*); advances + drives timers; calls
                                             FUN_140d6b0d0(singleton, time) (housekeeping) + FUN_140d6f310/f7a0.
CSScaleformSwfPlayer  (per-movie player)   struct size 0xe8 ; PRIMARY RTTI vtable er+0x2bbb360 (obj-offset 0)
                                             — only 2 vfuncs ([0] FUN_140d72030, [1] deleting dtor FUN_140d70740);
                                             so the player's advance/display are NON-virtual methods (0xd7xxxx band).
movie handle (from B3)                      WorldMapDialog+0x140 -> {MovieImpl* @+0x00 ; CSScaleformSwfPlayer* @+0x58}
                                             MovieImpl+0xA8 = int BufW,BufH (1920,1080); +0xB0 = clip L,T,W,H (descriptive, DEAD lever)
RTTI descriptor names (for per-boot re-resolve; ASLR-safe by name)
  ".?AVCSScaleformSwfPlayer@CS@@"           name @ er+0x3d01030  (TDbase 0x3d01020) ; COL @ er+0x335f8f8 (offset 0)
  ".?AVCSScaleformImp@CS@@"                 name @ er+0x3d00838  (TDbase 0x3d00828) ; COL @ er+0x335e7d0
```

Key structural facts for goal B:
- The player (`CSScaleformSwfPlayer`) is a small **0xe8-byte** object with only **2 virtuals** — the
  render/advance entry points are ordinary methods in the `er+0xd7xxxx` band, not vtable slots.
- `FUN_140d6b0d0` (called from the Step with a time delta) is **housekeeping**, not the draw — its deepest
  branch (`FUN_140d79d10`) merely sets the `System+0x1e19` reload flag. The real DISPLAY-submit is on the
  **render thread**, consistent with §4 of the render-toggle findings ("movies rasterize during present").
- No `SetVisible`/`GASPMovieRoot`/`bVisible` RTTI strings survive in this build (GFx internals are stripped),
  so the visible flag must be reached by offset from the render path or by live probe — not by name.

## 4. Goal B — the remaining lever + how to finish it (both routes)

The lever we want: a per-movie **render/visible** gate that the DISPLAY-submit consults, so MFG can, while
`vmap_covers_map()`, stop the world-map movie rasterizing WHILE its advance/logic keeps running (fast-travel,
page/cursor/fog all intact). Two concrete ways to pin it — do the live one first (B3's lesson: live RPM beat
static Ghidra for this exact movie):

**Route 1 (recommended, Linux-live RPM — reuse the `movieclip` scaffolding, now with a narrow target).**
The player is only **0xe8 bytes** and is already reachable live: `WorldMapDialog+0x140 → +0x58` =
`CSScaleformSwfPlayer*` (RTTI-confirmed). Extend the existing `worldmap_probe::movieclip_*` RPC to dump the
full **0xe8** player struct + the MovieImpl head, then A/B toggle candidate bool/int fields **one at a time**
across a map open/close while watching a screenshot for "pixels gone, logic alive." Prioritise byte/bool fields
that change between map-open and map-closed. This is the same method that cracked the clip rect in one session;
the search space is now one 0xe8 struct + the movie root, not a blind heap scan. (Flipping unknown fields can
crash — gate each write, snapshot/restore, one field per trial.)

**Route 2 (Ghidra — trace the render-thread DISPLAY-submit).** The Step (`FUN_140d6e6f0`) is the SIM-side
advance; the GPU submit is a separate render-thread pass over the same `System+0x9b0` movie list. To find it:
xref the CSScaleform singleton slot (`er+0x3d83148`) for a reader that runs off the graphics/present tick (not
the CSScaleformStep), or trace the non-virtual player methods in the `er+0xd7xxxx` band that call the vanilla
`Scaleform::GFx` movie **Display/Advance**; the byte the submit reads (per-player or on the GFx movie root) is
the gate. Anchor it by offset on the 0xe8 player and hand the offset to Route 1 for the live A/B.

**Preferred hook shape for MFG (once the gate is known):** per-present, while `vmap_covers_map()`, clear the
world-map player's render/visible byte (and restore on close) — mod-agnostic (resolve the player by RTTI +
movie-name `02_120_worldmap.gfx` per B3, never a hardcoded VA). If a callable `SetVisible`-style method turns
up instead of a raw byte, prefer calling it. **Do NOT** touch the update tick `FUN_140766980` or the menu
open/close (freeze risk).

**Guardrail confirmed by structure:** advance and display are separable here — advance is the CSScaleformStep
(sim tick, keeps the movie's animation/logic live) and display is the render-thread submit. Gating only the
submit leaves the movie advancing to a live-but-unshown state, which is exactly the "render-only off" the
design wants. (Verify on Linux that the map still closes normally and fast-travel/page/fog survive.)

## 5. Anchors
```
WorldMapDialog vtable   er+0x2b2d7d8  = 13 slots (enumerated §2); NONE submit the movie -> no draw vfunc
CSMenu base ctor        FUN_140741960 (er+0x741960)  stamps DLRefCountObj->SceneObjModifier->MenuWindow (1 vtable @off0)
menu UPDATE task        CSMenuManImp ctor FUN_1407650a0 registers ONE CSEzUpdateTask (this[0x10d]) -> exec FUN_140766980
                        (no second/draw task) ; FUN_140766980 = logic tick, LEAVE RUNNING
CSScaleform singleton   image slot er+0x3d83148 (CSScaleformImp) ; vtable er+0x2bb92f8
CSScaleformSystem       = *(CSScaleformImp+0x08) ; movie list @ +0x9b0 ; reload flag @ +0x1e19
CSScaleform Step exec   FUN_140d6e6f0 (er+0xd6e6f0)  StepTask<CSScaleformStep> (advance, NOT the GPU submit)
CSScaleformSwfPlayer    struct 0xe8 ; primary vtable er+0x2bbb360 (2 vfuncs) ; render/advance are non-virtual (0xd7xxxx)
movie handle (B3)       WorldMapDialog+0x140 -> +0x00 MovieImpl ; +0x58 CSScaleformSwfPlayer
DEAD levers             D3D12 scissor (no map rect) ; MovieImpl+0xB0 clip (descriptive) ; draw-vfunc slot (does not exist)
NEXT                    pin the per-movie render/visible gate: Route 1 live RPM on the 0xe8 player (fast) OR
                        Route 2 Ghidra render-thread DISPLAY-submit trace.
```
Cross-ref: `windows_native_map_render_toggle_re_findings.md` (§4/§4c/§4d — the two dead cheap levers),
`worldmap_native_clip_b3_scaleform_re_findings.md` (movieHandle chain + why live beat static),
`windows_native_map_drawvfunc_re_prompt.md` (this task's prompt).

## 6. Tooling note (headless Ghidra on this box)
- Drive it: `analyzeHeadless D:\ghidra_proj2 ER -process eldenring.exe -noanalysis -postScript <s>.java
  -scriptPath D:\ghidra_scripts` via PowerShell; capture `*> out.txt` then `iconv -f UTF-16LE -t UTF-8`
  (PowerShell redirection writes UTF-16LE). Load ~1–2 min/run.
- **The whole `-scriptPath` dir compiles as ONE OSGi bundle** — a compile error in ANY `.java` there makes
  EVERY script fail with a misleading `"class could not be found / Failed to get OSGi bundle"` (no javac
  diagnostic reaches stdout or the app log). If a script that worked yesterday suddenly won't load, a sibling
  is broken. Fastest triage: compile the one file with Windows `javac` against a jar glob
  (`Get-ChildItem D:\ghidra\...\*.jar | ?{$_.Name -match 'Base|Decompiler|Docking|Generic|SoftwareModeling|Utility|Project|Gui'}`)
  to get the real error. (Here: a helper called `rva(Address)` but only `rva(long)` was defined.)
- MSVC RTTI COLs use 32-bit **RVAs**, so Ghidra's reference manager shows **no** xref from a COL to its type
  descriptor — resolve vtables by raw byte search: find the 4-byte `TDbase-RVA` (COL.pTypeDescriptor), take
  `COLbase = hit-0xC`, require `*COLbase==1` (x64 sig) and read `COLbase+4`==0 for the primary; then find the
  8-byte absolute `COLbase` pointer = `vtable[-1]`, so `vtable = hit+8`. The TD **name** field starts at the
  `.?AV` prefix (4 bytes before the class-name substring).
```
