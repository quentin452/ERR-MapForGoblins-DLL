# RE findings — enumerate ALL resident item-icon sprites in one pass (FD4 image-repo walk)

Answers `docs/re/windows_resident_icon_enumeration_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v96`/`re_v97`, logs alongside). App 2.6.2.0 / ERR 2.2.9.6,
imagebase `0x140000000`. Read-only. Supersedes the §8 movie-walk in
`windows_runtime_item_sprite_re_findings.md` (that targeted load-screen movies on this build).

**✅ VALIDATED LIVE (2026-06-22, 2.6.2.0/Proton).** CE table walk with inventory open:
`repo=0x…DC0200`, `_Mysize=938` (all resident images), `walked=938`, **`MENU_ItemIcon_*=163`** — all
`vt=OK`, rects + sheet groupings correct (e.g. crafting-mat 415xx all share one 4096-wide sheet, 45xxx
sorceries another). The container recipe (§1) is confirmed. **One correction:** the DXGI format is NOT
at `sheet+0x30` — that reads `0` live for every entry (and the in-DLL `cache_icon_from_img` reads the
same offset; `goblin_inject.cpp:2349` already carries a TODO that the internal-format offset is
unidentified). The format offset is a **separate, still-open sub-task** (probe `sheet+0x00..0x100` for
the W/H/format triple) — it does NOT affect enumeration, only downstream drawing.

Deliverables shipped:
- **Cheat Engine table:** `tools/cheat_engine/MapForGoblins_icon_repo.CT` — resolves the repo from the
  static anchor, walks the tree, lists every resident `MENU_ItemIcon_*` (name + image + rect + sheet +
  format) live; press **NUMPAD 2** with the inventory open.
- **Runtime validator:** `D:\ghidra_scripts\walk_icon_repo.py` — Python ctypes RPM walk, same recipe,
  cross-checks rect/sheet/format against the known-correct find-hook values.
- This findings doc (the stable offset recipe + validation plan).

---

## 0. TL;DR

The `repo` we already hold (`FUN_140d63c30` arg0, stashed as `g_icon_repo`; the static singleton is
**`DAT_143d82510`** = `er+0x3d82510`) is an **`FD4ResRep`/`FD4ResCapHolder`** whose by-name lookup is
a plain **MSVC `std::map<DLWString, CS::CSTextureImage*>` (red-black tree) at `repo+0x80`**. Walking
that tree enumerates **every currently-resident image** — including all loaded `MENU_ItemIcon_<id>` —
in one pass, **and the node stores the `CSTextureImage*` value directly (no re-resolve-by-name
needed)**. This is the proactive full-harvest the §8 movie-walk failed to deliver on this build.

The map is keyed by the **full GFx symbol string** (`MENU_ItemIcon_<id>`, ERR `MENU_FL_<id>`, plus
`KG_*` / `SB_ERR_*` etc.), so filtering to icons is just a `MENU_ItemIcon_` name-prefix test on the
node key. **Residency limit is unchanged**: the walk yields only icons whose sheet the engine has
loaded; force-load (to cover un-browsed items) is still a separate, lower-priority lead (§5).

---

## 1. The repository container (instruction-confirmed, re_v96 + re_v97)

`FUN_140d63c30(repo, void** out, const wchar_t* key)` (`er+0xd63c30`) is `FD4ResRep::find` /
operator[]-style by-name lookup. The relevant disasm (`re_v96_out.log`):

```
d63d11:  LEA  RCX,[R15+0x80]          ; RCX = &repo._Tree   (the std::map object)
d63d18:  CALL FUN_140d62c10           ; node = lower_bound(&_Tree, &keyDLWString)
d63d1d:  MOV  R14,RAX                 ; R14 = candidate node
d63d20:  MOV  RBX,[R15+0x88]          ; RBX = *(repo+0x88) = _Myhead == end()
d63d27:  CMP  RAX,RBX / JZ ...        ; node == end()  -> not found
...      (verify node key == requested key via wcsncmp FUN_14011e1f0)
d63d8b:  MOV  RBX,[R14+0x50]          ; value  = *(node+0x50)  = CS::CSTextureImage*
d63d97:  CALL <addref>                ; refcount++ (thunk_FUN_141112b7b0)
d63da9:  MOV  [RDI],RBX               ; *out = value
```

The lookup `FUN_140d62c10` (`er+0xd62c10`) is a textbook MSVC `_Tree::lower_bound`:
```
puVar9 = *(this+8)            ; _Myhead          (this = repo+0x80  ->  _Myhead at repo+0x88)
node   = *(_Myhead+8)         ; root = _Myhead._Parent
loop while *(u8)(node+0x19)==0:    ; _Isnil flag
    cmp key vs *(node+0x28) ...    ; node key DLWString @ node+0x28 (len @+0x38, cap @+0x40)
    node = *(node+0x00)  (Left)  OR  *(node+0x10)  (Right)
```

### Stable layout (RVAs drift under VMProtect — resolve fns by AOB; the chain shape + offsets hold)

```
repo            = *(eldenring.exe + 0x3D82510)        ; DAT_143d82510, FD4Singleton<image repo>
  repo + 0x80   = MSVC std::map _Tree object          ; pass &(repo+0x80) as `this` to the lookup
  repo + 0x88   = _Myhead  (nil sentinel)  == end()
  repo + 0x90   = _Mysize  (count of ALL resident images; not just icons)
  root          = *(_Myhead + 0x08)                   ; _Myhead._Parent

ResCap node (MSVC _Tree_node):
  node + 0x00   = _Left
  node + 0x08   = _Parent
  node + 0x10   = _Right
  node + 0x19   = _Isnil   (u8; 1 on the sentinel — skip those while walking)
  node + 0x28   = key DLWString : heap buf ptr @ +0x28 IFF capacity(@node+0x40) >= 8, else inline
                                  wchars @ +0x28 ; length(wchars) @ node+0x38
  node + 0x50   = value = CS::CSTextureImage*          ; DIRECT — no find-by-name re-resolve

CSTextureImage (already known, re-confirmed): vtable == eldenring.exe + 0x2BB8910 (validity guard)
  img + 0x74/0x78/0x7c/0x80 = sub-rect x0/y0/x1/y1
  img + 0x10 -> Render::Texture, +0x70 -> ID3D12Resource (the BCn sheet)
  sheet res: DXGI_FORMAT offset UNIDENTIFIED (res+0x30 reads 0 live — was a guess; probe res+0x00..0x100
             for the W/H/format triple; vkd3d-internal on Proton). Does not affect enumeration.
```

### Enumeration recipe (the walk)

In-order is unnecessary — a stack DFS over `_Left`/`_Right`, skipping any node with `_Isnil != 0`,
visits every entry:
1. `repo = *(er+0x3D82510)`; `head = *(repo+0x88)`; `root = *(head+0x08)`.
2. DFS from `root`: for each node with `*(u8)(node+0x19)==0`, read `name = DLWString(node+0x28)`.
3. Filter `name` by prefix `"MENU_ItemIcon_"` → `iconId = atoi(name+14)`.
4. `img = *(node+0x50)`; guard `*(img) == er+0x2BB8910`; read rect `img+0x74..0x80`, sheet
   `*( *(img+0x10) + 0x70 )`, format `sheet+0x30`. Cache per iconId — **no name re-resolve**.

The twin `FUN_140d63e50` (`er+0xd63e50`) is the SAME find against a **second** map at `repo+0xb0`
(end `repo+0xb8`, lookup `FUN_140d62ce0`) — a separate resource category. Item icons are in the
`repo+0x80` map (the one the icon widget `FUN_14074bcc0` and `FUN_140d7c940` query via `DAT_143d82510`).

### Icon filter

By **name prefix** on the node key (`MENU_ItemIcon_`). The map mixes all GFx image symbols
(`MENU_FL_*`, `KG_*`, `SB_ERR_*`, load-screen art, …); the prefix test is self-validating and matches
exactly what the existing find-hook caches.

---

## 2. The static anchor for the CE table (re_v97)

`FUN_140d7c940` (`er+0xd7c940`) is the clean global-form caller:
```c
if (DAT_143d82510 == 0) <FD4Singleton not-instantiated assert>;
FUN_140d63c30(DAT_143d82510, out, key, ...);
```
So the anchor is the **static data pointer `eldenring.exe + 0x3D82510`** (`DAT_143d82510`); the repo
object = `*(er+0x3D82510)`. `.data` RVAs are stable across this VMProtect build (code is virtualized,
the singleton slot is not), so the CT uses `eldenring.exe+3D82510` directly. If a future build moves
it, AOB the `FUN_140d7c940` callsite (the `MOV RCX,[rip+DAT_143d82510]` feeding the `CALL
FUN_140d63c30`) and read the rip-relative disp — the find fn itself is reachable by its existing AOB.

---

## 3. FUN_140d69640 scope — confirmed NOT the item enumerate on this build

Per the brief's question: `FUN_140d69640` (`er+0xd69640`) is a **caller** of the find (re_v96 caller
list: `@0xd696f5 in FUN_140d69640`) — it walks ONE loaded movie's image-list and resolves each name
via `FUN_140d63c30`. The live `[WALK-DIAG]` showed it firing only for **`MENU_Load_*` movies
(count=1)**, and the static `res+0x88==4` type-gate read `0x44` at runtime. It is a **per-movie
find-one**, load-screen-scoped on this build — NOT the inventory item enumerate. **The repo-walk
(§1) supersedes it**: it is movie-independent and covers every resident image regardless of which
movie loaded it, so we do not need an inventory-movie enumerate at all.

---

## 4. Validation plan (run with the game open + inventory up)

The repo only populates once a menu has loaded icon sheets — open inventory/equipment first.

1. **CE table** (`MapForGoblins_icon_repo.CT`): load it, open the inventory, press **NUMPAD 2**.
   - Expect: `_Mysize` (entry #3) > 0 and growing as you browse; `MENU_ItemIcon_*` count (entry #4)
     in the hundreds; the console/`MapForGoblins_icon_repo_ct.log` lists names + `vt=OK` + rects +
     sheet. (Done: 163 icons / 938 images, `vt=OK`, rects+sheets correct. `fmt` reads 0 — format
     offset open, §0.)
2. **Python RPM** (`python D:\ghidra_scripts\walk_icon_repo.py`): prints `total nodes`,
   `MENU_ItemIcon_* entries`, and ~12 samples.
3. **Cross-check (proves correctness):** pick a few iconIds present in BOTH the walk output and the
   find-hook `[ENUM2]` log (`dump_icon_textures=true`) — their rect `(x0,y0)-(x1,y1)`, sheet
   resource ptr, and DXGI format must match (the find-hook values are known-correct, sprite-findings
   §7/P2b). With N icons resident, the walk must yield exactly the N `MENU_ItemIcon_*` keys.

Once validated live, point the in-DLL proactive harvest at this walk: in
`src/goblin_inject.cpp`, add a `harvest_repo_icons()` that DFS-walks `*(g_icon_repo)`-anchored… —
actually walk from `*(er+0x3D82510)` (the canonical repo) using the §1 offsets, reading
`*(node+0x50)` directly and feeding `cache_icon_from_img` (skip the name re-resolve entirely). Run it
on a menu-open tick (replaces / augments the §8 `harvest_resident_icons` movie-walk). RPM-safe,
read-only, no `__try` needed (pointer-guarded reads only).

---

## 5. Secondary — force-load (INVESTIGATED re_v98–v100; NO clean runtime lever)

The walk is resident-only — it yields only the icons whose atlas the current menu loaded ("TOUT du
menu, pas TOUT du jeu"). To cover un-browsed items the atlas TPF must be made resident first. The
full load path was reversed; the verdict is that **a safe, context-free force-load does not exist**:

- **The find/widget never loads.** The icon widget `FUN_14074bcc0` does per-icon find
  (`FUN_140d63e50`, the `repo+0xb0` map) → on miss the sheet-key find → on miss it simply **draws
  nothing**. No load is triggered by lookup.
- **`CSScaleformImageCreator::CreateImage` `FUN_140d6bbc0`** builds an FD4 filepath from the symbol
  (`FUN_140d6ba60` + format `DAT_142a91a00`) and calls the repo loader `FUN_140d64490`, but that only
  creates a per-symbol image **VIEW** (the `<symbol>_ptl` descriptor, `L"%s_ptl"`) into an atlas that
  must ALREADY be resident — it does not stream the atlas TPF. And CreateImage is a GFx image-creator
  **callback** (no static callers); calling it needs the live Scaleform/`GFxMovieView` context.
- **The atlas TPFs are streamed by the menu resource orchestrator `FUN_140d790a0`** (3044 B; it builds
  the path via `FUN_140d7bb10` — `menu:/%s%s.tpf`, `menutpfbnd:/00_Solo/%s.tpf`, `…/71_MapTile/…`
  selected by movie-name prefix — then drives the FD4 task/streaming system). Driving it from our DLL
  means supplying a menu/movie instance + the task system on the right thread → fragile, high crash
  risk (matches sprite-findings §3).
- **`"RequestReloadMenuTexture"`** (`0x2bd6b60`) looked promising but is referenced **only** by a
  command-name data table (`0x3b3fee8`) with **no statically-reachable handler** (VMProtect'd /
  data-driven) — not callable.
- Even if the atlas TPF were force-streamed, the repo only gains `MENU_ItemIcon_<id>` `CSTextureImage`
  entries once **GFx binds the movie** (menu init processes the `sblytbnd` layout). So populating the
  repo for un-browsed items effectively requires re-running menu init — not a bounded, safe call.

- **The orchestrator is a per-frame TICK, not a by-name entry** (re_v101). `FUN_140d790a0` is ticked
  by `FUN_140d724c0`/`FUN_140d78060` (the CSMenuResourceManager update) and drains a **load queue**
  (`manager+0x1df0`/`+0x1e08`) fed by menu-open requests — for each queued movie it calls our known
  find (`FUN_140d7c940`) + enumerate (`FUN_140d69640`). So a force-load = locating the live manager
  singleton AND constructing valid movie-load requests AND letting the FD4 task system drain them on
  the right thread — i.e. re-implementing menu loading. No bounded by-name call exists.

**Conclusion / recommendation.** Runtime full-coverage is not safely achievable without re-driving
menu/GFx init. The robust path to 100% coverage is **offline extraction** (`tools/extract_subtextures.py`
already crops sub-textures from the `sblytbnd.dcx` layout + DDS atlases, vanilla AND ERR) → a baked
per-iconId atlas, with the §8b runtime repo-walk kept as the live/mod-freshness override. Handles for a
future attempt are preserved above should a cleaner streaming entry surface.

---

## 5b. The texture-manager LOAD lever (re_v104–v109) — callable

Reframed force-load attempt (the "texture manager" angle). The file/resource loader IS exposed and
callable without a menu, via the **`CSFile` singleton**:

- **`CSFile` singleton = `*(er + 0x3D5B0F8)`** (`DAT_143d5b0f8`, an `FD4DerivedSingleton<CSFile>`).
- **load-by-path family** `FUN_1401f5560` / `FUN_1401f5090` / `FUN_1401f2300` (`er+0x1f5560` etc.).
  Disasm (re_v109): standard `__fastcall`, saves RCX/RDX/R8/R9, allocates a 0x98-byte request, then
  fills+enqueues it (tail truncated by Ghidra after the alloc thunk `FUN_141eb9ed0(0x98,8)`):
  ```c
  void* load(void* CSFile /*RCX*/, const wchar_t* path /*RDX*/, void* a2=0 /*R8*/, uint32_t a3=0 /*R9*/);
  ```
  In every caller the 2nd arg is the **raw `wchar_t*` path buffer** (extracted from an FD4 path obj),
  so a plain `L"menu:/….tpf"` works. `FUN_1401f5560` is the variant the **resident-group** loader uses.
- **resource-GROUP loader** `FUN_140d77550(menuResMgr, groupId)` (`er+0xd77550`): indexes a name table
  (`PTR_…00_Solo…142bbb4e0`), builds `menu:/<name>.tpf`/`.tpfbhd`, loads via CSFile. Path templates:
  `menu:/%s.tpf`, `menu:/%s%s.%s`, `menutpfbnd:/00_Solo/%s.tpf`, `menutpfbnd:/71_MapTile/%s.tpf`.
- The Scaleform resident-load steps that call all this: `STEP_Init_forResidentTextureLoad`
  `FUN_140d6e1c0`, `STEP_Init_forResidentResourceLoad` `FUN_140d6e190`, `STEP_Init_forGfxFileLoad`
  `FUN_140d6e130` (table built in `FUN_1400ae9e0`). The fixed 113-resource menu set loads in
  `FUN_140d6ae30` (table `DAT_143b3d360`).

**Caveat (unchanged):** loading a TPF gives the **atlas texture**; the per-icon **rects** come from the
gfx layout (the repo, §1) which needs the gfx movie bound. So a TPF force-load alone = atlas without
rects → pair with offline sblytbnd rects, OR only needed for the 63 WorldMapPoint pin frames (which
live in the world-map gfx, resident when the map is open).

**The menu file list (re_v110).** The engine uses two FIXED (hardcoded) tables — no dynamic manifest,
but mod-robust because mods repackage the same-named BNDs:
- **`PTR_142bbb4e0` — the 9 resource-GROUP BNDs** (`FUN_140d77550(ctx, groupId)` builds
  `menu:/<name>.tpf`/`.tpfbhd`): `[0] 00_Solo  [1] 01_Common  [2] 02_Title  [3] 03_ChrMake  [4]
  04_NowLoading  [5] 05_Dummy  [6] 06_Platform  [7] 71_MapTile  [8] 80_Language`. **Item-icon atlases
  are TPFs inside these group BNDs (likely `01_Common`); `03_ChrMake` = the face/beard sprites seen
  resident in char-creation.**
- **`DAT_143b3d360` — 7 always-resident common gfx** (`FUN_140d6ae30`, loaded `menu:/Win/<name>.gfx`):
  `01_900_Black`, `01_080_EmergencyNotice`, `01_090_SummonMessage`, `01_910_Fade`, `01_920_Movie`,
  `01_930_KeyGuide` (path builder `FUN_140d7b800` = `menu:/Win/%s.gfx`).

So force-load targets = the group BNDs (`menu:/01_Common.tpfbhd` etc.) via CSFile, OR a specific TPF
inside a BND. The exact per-atlas TPF `<name>` lives in the group-BND TOC (read at runtime or offline).

**Force-load CALL works, but is async + insufficient alone (2026-06-22).** Sweeping the group BNDs
(`menu:/01_Common.tpfbhd`, `00_Solo`, `03_ChrMake`, `71_MapTile`, `02_Title`, …) each returned a
**non-null handle, no crash** — BUT the handles are sequential same-heap allocations (`…f8a0 …f800
…f6c0 …f580`, 0x80 apart) = the **0x98 async REQUEST object** the loader allocates immediately. So a
non-null return does NOT prove the file loaded (it's the request, the load is async/queued). And
`harvested` stayed 131→131 across all of them: **loading a TPF/BND yields the atlas bytes, NOT the
per-icon repo entries (rects)** — those appear only when the gfx movie BINDS (menu init processes the
sblytbnd layout). Confirmed live.

**Bottom line on the texture-manager lever:** callable from the DLL, but (a) success isn't observable
from the return (async), and (b) a successful TPF/BND load still gives an atlas WITHOUT per-icon rects.
Runtime force-load can stream the atlas; the rects must come from the gfx binding (fragile) OR offline
`sblytbnd` (`extract_subtextures.py`). For the map's 63 WorldMapPoint pins (category design) no
force-load is needed — they're in the world-map gfx, resident when the map is open.

**Open (next, if pursuing per-item icons):** poll the request object's state to confirm load success;
read the loaded sblytbnd layout at runtime for rects; OR bake rects offline + force-load atlas.

**Test harness:** `goblin::force_load_file(path)` (dev, `dump_icon_textures`) calls
`load(CSFile, path, 0, 0)` from a P2b-panel button — see `src/goblin_inject.cpp` / `goblin_overlay.cpp`.

---

## 5c. The GFX BINDING — rects come from the sblytbnd, not the TPF (re_v111–v115)

How loaded resources become findable `MENU_ItemIcon_<id>` repo entries WITH rects:

1. **sblytbnd = the layout (rects + names), loaded per group.** `FUN_140d771d0(menuCtx, groupId)`
   (`er+0xd771d0`) builds `menu:/<group>.sblytbnd` and loads via CSFile `FUN_1401f5a00`, storing the
   handle at `menuCtx+0xd58+groupId*8`. The layout holds per-sprite **(name, rect[x0,y0,x1,y1],
   sheet-ref)**. (The TPF atlas — loaded separately — is just the pixels.)
2. **Per-sprite create:** `FUN_140d650b0(layoutObj, name, rect*, sheet, …)` (`er+0xd650b0`) creates a
   `CS::CSTextureImage` via `FUN_140d68410` (`er+0xd68410`), which **writes the rect**:
   `img+0x74=rect[0]  +0x78=rect[1]  +0x7c=rect[2]  +0x80=rect[3]`, sets the name (`img+0x40` region),
   the sheet dims (`+0x2c/+0x6c`) and a flip flag. No-rect variant `FUN_140d68550`.
3. **Insert into the image map by name:** `FUN_140d63060` / `FUN_140d62db0` (`er+0xd63060`/`0xd62db0`)
   on a `std::map` at `obj+0x98` (end `obj+0xa0`) — same shape as the repo's `repo+0x80`; and
   `FUN_140d68690`→`FUN_140d65cf0(repo=DAT_143d82510, …)` inserts into the global repo.
4. **The parse loop** (function at `er+0xd66520`, body loop ending `0xd66519`; `this`=layoutObj in R13,
   its map at R13+0x98 / end R13+0xa0) iterates the layout entries → create (2) + insert (3) per sprite.
   This region was UNDEFINED in Ghidra (VMProtect/indirect) — disassembled in re_v115.

**Key insight:** the per-icon **rects live in the `sblytbnd`**, and the binding parses it to build the
repo entries. So a TPF-only force-load gives pixels without rects (the §5b wall); the runtime path to
per-item icons WITHOUT the menu is: force-load the group **sblytbnd** (`menu:/<group>.sblytbnd`) +
trigger this parse loop → repo gains `MENU_ItemIcon_<id>` with rects → harvest as usual.

**Trigger RESOLVED (re_v116):** the binding fns are **virtual methods** — `FUN_140d650b0` (create) and
`FUN_140d66520` (find-or-create) are referenced as DATA in two vtables (`0x493c8f0`/`0x493c9bc` and
`0x378bf40`/`0x378ba40`) plus one direct call (`@0xd64689`, also undefined region). So the parse is
invoked **polymorphically when the texture-provider/sblytbnd resource is applied** (menu load path) —
NOT a standalone callable. Triggering it by hand = constructing/holding a valid provider object + its
apply sequence → as fragile as driving the menu orchestrator (§5).

**Conclusion — don't trigger the runtime binding.** The per-icon **rects live in the `sblytbnd` file**,
which `tools/extract_subtextures.py` already parses OFFLINE. So per-item icons = **offline rects
(sblytbnd) + pixels from the TPF atlas** (offline bake, OR runtime force-load the TPF via the §5b
CSFile lever + offline rects). This short-circuits the fragile runtime binding entirely.

---

## 5d. WHAT governs residency — the menu resource manager (re_v117–v118)

The object that decides which icons are loaded/evicted is the **menu resource manager** (a
`CSMenuResourceManager`-like, reached via the task system — `FUN_140d79d90` gets it as param_1 and
ticks it; context getters `FUN_140d69a60`/`FUN_140d69c20`). It is NOT a flat global. Layout:

```
manager:
  +0x9d8   array of loaded resource GROUPS (movies/bundles)  (count @+0xbe0, stride 0x10)
  +0xda0   lock
  +0x1df0  LOAD queue        (drained by FUN_140d790a0 er+0xd790a0)
  +0x1e08  UNLOAD/evict queue (drained by FUN_140d790a0)
  +0x1e19  dirty flag → triggers queue processing (FUN_140d790a0)
  per-tick update: FUN_140d78540 (er+0xd78540) loops +0x9d8; entry+0x7c!=0 → FUN_140d70940 (load/evict)
  tickers: FUN_140d724c0 (er+0xd724c0), FUN_140d78060 (er+0xd78060)
```

It governs which **resource GROUPS/movies** are resident (= which menu pages are in memory); **icons
follow their group** (a group loads → its icons enter the repo via the binding §5c; a group evicts →
its icons leave — the `_Mysize` churn the live monitor showed). The repo `DAT_143d82510` is the
resulting passive container.

**Live monitor data (browse-to-fill ceiling):** the game streams a window of **~150** item icons; the
session-UNION of distinct `MENU_ItemIcon` grew 151→216 over ~1 min of browsing (bursts of +11..+14 on
new tabs/menus). So browse-to-fill accumulates but reaching ALL game items by pure browsing is
impractical (thousands). The repo+0x80 map is the right one (twin repo+0xb0 has 0 item icons).

**Manipulation levers (if pursued):** grab the manager live by hooking a ticker (`FUN_140d724c0`,
capture `*param_2`), then either enqueue loads (+0x1df0 + set +0x1e19 — but building valid request
entries is complex) or **block the unload/evict** (+0x1e08 drain, or neuter `FUN_140d70940`'s evict)
so the repo ACCUMULATES the browse union instead of evicting — the safer of the two to try.

---

## 5e. The ticker chain decompiled — force-load lever is NOT in the ticker; bind = a flag flip (re_v119)

Decompiled the whole residency tick chain (`find_resmgr.java` / `find_resload.java`,
`D:\ghidra_scripts\out_resmgr.txt` / `out_resload.txt`). Answers "can a non-resident item icon be
forced resident via `FUN_140d724c0`?" — **the ticker is only the pump; the levers are downstream.**

**The ticker `FUN_140d724c0(unused, &manager)` (RVA 0xd724c0)** — `manager = *param_2`:
```c
if (FUN_140d69d00() == 0) {                       // not in the alt/teardown branch
  if (*(char*)(manager + 0x1e19) != 0) {          // dirty flag set?
    FUN_140d790a0(manager, param_2 + 1);          //   -> drain LOAD(+0x1df0)/UNLOAD(+0x1e08) queues
    *(char*)(manager + 0x1e19) = 0;               //   -> clear dirty
  }
  FUN_140d78540(manager);                          // ALWAYS: per-tick group apply
}
```
Sibling ticker `FUN_140d78060` (RVA 0xd78060) does the same under a lock (`mgr = *(param_1+0x18)`).
So the ticker contains **no load logic** — it only (a) drains queues when `+0x1e19` is dirty, and
(b) runs the per-tick group apply.

**The LOAD queue `+0x1df0` (drained by `FUN_140d790a0` pass-1):** a linked list of **movie-load
REQUEST objects** (`req = node[2]`). Per request: not-loaded flag `req+0x48`; movie count
`FUN_140d695e0(req)`; movie key `FUN_140d69590(req,i)`; for each key `FUN_140d7c940` (our known repo
find) then apply; done-check `FUN_140d69630(req)`; pump `FUN_140d69580(req, *(param_2+8))`; on done,
unlink + `mgr+0x1df8`--. **Confirms §5d:** forcing via this queue = fabricating a valid request object
(the movie set + its load state machine) — complex, not pursued.

**The FILE-LOAD half — cleanly callable by groupId (the real §5b/5c levers, exact sites):**
- `FUN_140d77550(manager, byte groupId)` (RVA 0xd77550) — loads the group **TPF** (`menu:/<name>.tpf`
  / `.tpfbhd`, name from `PTR_u_00_Solo_142bbb4e0[groupId]`) via CSFile `FUN_1401f5560`/`FUN_1401f52f0`;
  **guarded** `if (*(manager + groupId*8 + 0xd10) == 0)` (handle cached at `manager+0xd10+gid*8`).
- `FUN_140d771d0(manager, byte groupId)` (RVA 0xd771d0) — loads the group **sblytbnd**
  (`menu:/<name>.sblytbnd`, the layout = the per-icon **rects**) via CSFile `FUN_1401f5a00`; guarded
  `if (*(manager + groupId*8 + 0xd58) == 0)` (handle at `manager+0xd58+gid*8`).
- Wrappers (`step+8` = manager), all called by STEP_Init `FUN_140d6e1c0`/`FUN_140d6e320`:
  `FUN_140d6ae40`→`77550(mgr,8)`; `FUN_140d6ae60`→`77550(mgr,4)`; `FUN_140d6b000`→`77550(mgr,2)`;
  `FUN_140d6ae80`→`77550(mgr,id)` over table `DAT_142bb9120..142bb9125`; **`FUN_140d6aef0`→`771d0(mgr,
  {1,2,3,4,5,6,8})`** (the sblytbnd set). `FUN_140d6ae30` = the 113-resource `.gfx` set (`DAT_143b3d360`).
- **These wrappers ONLY load files** — none calls a bind/apply (no `FUN_140d70940`, no `+0x7c`, no
  vmethod `+0xc8`). So loading the sblytbnd via this path does **not** populate the repo with rects —
  surgical confirmation of the §5c wall, now with the exact call sites.

**★ NEW — the BIND has a per-tick trigger that is a FLAG FLIP, not a fabricated request.**
The per-tick apply `FUN_140d78540(manager)` (RVA 0xd78540) loops the **group array** (`manager+0x9d8`,
stride 0x10, count `manager+0xbe0`) and for any entry with **`entry+0x7c != 0`** calls
`FUN_140d70940(entry)` (RVA 0xd70940):
```c
FUN_140d70940(entry):
  entry+0x7c = 0;                                            // consume the dirty flag
  if (entry+0x18 != 0) {                                     // entry+0x18 = the group RESOURCE object
    ... FUN_140d717f0(entry,&key) ...                        // resolve the group key
    (**(code**)(*(entry+0x18) + 0xc8))(entry+0x18, 1);       // <<< the APPLY/BIND vmethod (+0xc8)
    entry+0x98 += 1;                                         // apply counter
    entry+0x80 = 1;                                          // applied flag
  }
```
So the bind we need (§5c: the sblytbnd parse that emits `MENU_ItemIcon_<id>` repo entries with rects)
is reachable by **setting a registered group entry's `+0x7c` flag** → next tick → vmethod `entry+0x18
→ +0xc8`. This is far simpler than the §5d "enqueue into +0x1df0" path (no request-object synthesis).

**Group entry layout (`manager+0x9d8 + i*0x10` → entry):** `+0x18` resource obj (vtable `+0xc8` =
apply/bind), `+0x38` lock, `+0x7c` dirty/needs-apply (int), `+0x80` applied flag, `+0x88` lock,
`+0x98` apply counter. `manager+0xbe0` = group count; `manager+0xda0` = the manager lock.

**Decision — the remaining unknown is best closed at RUNTIME (quentin's workflow).** Two facts must hold
for the flag-flip to yield per-item icons, neither statically resolvable (resource obj is built at
runtime so vtable+0xc8 can't be pinned in Ghidra; group registration is dynamic):
1. the item-icon group (likely **gid 1 = 01_Common**) is **registered** as a `+0x9d8` entry even when
   un-browsed (or can be made so by the file-load half), and
2. its `entry+0x18 → vtable+0xc8` apply is the sblytbnd parse (`FUN_140d650b0`/`FUN_140d66520` family,
   §5c) that writes repo rects — not a generic "mark resident" that still needs the menu's own apply.

**Gated runtime experiment (next, quentin):** with the manager pinned live (hook a ticker
`FUN_140d724c0`, capture `*param_2`; or resolve via context getter `FUN_140d69c20`/`FUN_140d69b40`):
(a) ensure files resident — `FUN_140d77550(mgr,1)` + `FUN_140d771d0(mgr,1)`; (b) walk `mgr+0x9d8`
[count `mgr+0xbe0`], find the 01_Common entry, log its `entry+0x18` vtable + the `+0xc8` target;
(c) set `entry+0x7c = 1`, set `mgr+0x1e19 = 1`, wait a few ticks; (d) re-walk the repo (`harvest_repo_icons`)
— did `_Mysize` / distinct `MENU_ItemIcon` jump for un-browsed items? Gate it like `force_load_file`
(dev panel button), VRAM-watch, single group at a time. If the repo gains rects → this is the runtime
100%-coverage lever; if not → vmethod `+0xc8` is not the parse and offline bake (§5/§5c) stays the path.

**"bind/binding" string sweep (re_v120, `find_bindstr.java` / `out_bindstr.txt`).** Searched defined
strings for binding/layout/provider/apply/resident/sprite terms + xref source. Useful artifacts:
- **`ShoeboxLayoutbndFileCap`** (RVA 0x29dc450, name-reg `FUN_1400801f0`) — the **sblytbnd resource
  capsule class** ("ShoeBox LaYouT bnd" = sblytbnd). This is the resource object whose APPLY parses the
  layout into `CSTextureImage` repo entries (§5c) — i.e. the likely type of `entry+0x18` for the
  item-icon group, so `entry+0x18 → vtable+0xc8` is its layout-parse apply. **`GfxFileCap`** (0x29d8610,
  `FUN_14007f3f0`) = the .gfx capsule. (Both fns only register the RTTI type-name at desc+0x38/+0x40;
  the concrete FileCap vtable is factory/VMP-built → resolve `+0xc8` at runtime via the dump-groups test.)
- **`CSScaleformStep::STEP_Init_forResidentTextureLoad` / `..._forResidentResourceLoad` /
  `..._forGfxFileLoad` / `..._forResidentTextureRELOAD`** (all `FUN_1400ae9e0`, the §5b STEP table) —
  note the explicit **Reload** step (a built-in re-apply path worth a look if the flag-flip misbehaves).
- `menu:/%s.sblytbnd` ← `FUN_140d771d0` (confirms our loader); `Failed to bind SWF file "` ←
  `FUN_141162d30` (the low-level GFx/Scaleform movie bind). The bulk of `*Binding*` hits are Havok
  (`hkbVariableBindingSet`, animation/skin binding) — irrelevant to menu icons.

**DLL test SHIPPED (commit, 2026-06-22).** `goblin::bind_test(action, gid)` + P2b panel buttons, behind
`config::dumpIconTextures`. Hooks the ticker `FUN_140d724c0`, captures the live manager (`*param_2`),
runs one-shot actions inline on the engine thread before the original: (1) dump groups (logs each
loaded group's `entry+0x18` resource + `apply(vt+0xc8)` RVA + `+0x7c`/`+0x80` flags), (2) load files by
gid via `FUN_140d77550`+`FUN_140d771d0` (handle cached at `mgr+0xd10/0xd58` — where the bind looks),
(3) flip-bind all (set `+0x7c=1` + dirty `+0x1e19`), (4) load+flip. RUNTIME RESULT (quentin, 2026-06-22,
`logs/MapForGoblins.log`): **the +0x7c flag-flip is REFUTED as a bind lever.**
- Manager captured (`mgr=0x22280302080`), **15 group entries**. All 4 actions (dump / load gid1 /
  flip-bind / load+flip) left **`harvested` flat at 162→162** — the repo gained NO entries.
- **Every group's apply `vt+0xc8` = the SAME fn `FUN_14112fc80` (RVA 0x112fc80)** — and decompiling it
  (`find_apply.java`) shows it is a **Scaleform display-tree RENDER/update** method (`Matrix2x4<float>`,
  `Scaleform::Render::Matrix3x4::MultiplyMatrix`, walks `obj+0x30` siblings from root `param_1+0x90`),
  NOT a resource load/bind. So the manager's `+0x9d8` "groups" are the **15 loaded Scaleform MOVIES**,
  and `FUN_140d70940`'s `+0xc8` call is the movie's per-tick render-update — flipping `+0x7c` just
  re-renders the movie, it does not re-parse any sblytbnd into repo rects.
- Loading files by gid (`FUN_140d77550`/`771d0`) likewise added nothing (loading ≠ binding, §5b).

**CONCLUSION — runtime force-residency via the residency manager is exhausted and refuted at every
accessible trigger:** (1) force-load files → atlas/sblytbnd bytes, no rects; (2) `+0x7c` group-apply →
movie render, no rects; (3) `+0x1df0` request → same movie-apply machinery. The per-icon rect-binding
is ONLY the menu-init virtual apply on the `ShoeboxLayoutbndFileCap` (vtables 0x493c8f0/0x378bf40,
§5c) — not reachable as a bounded call. **The robust 100%-coverage path is OFFLINE BAKE** (extend
`tools/extract_subtextures.py`, which already parses the sblytbnd, to emit iconId→(atlas,rect)); keep
the runtime repo-walk (`harvest_repo_icons`) as the live/mod-freshness override. This is now an
empirical conclusion, not a theoretical one. (The `bind_test` dev harness stays as a gated diagnostic.)

---

## 6. Handles
- repo singleton `DAT_143d82510` `er+0x3D82510` (FD4Singleton); container map `repo+0x80`,
  `_Myhead` `repo+0x88`, `_Mysize` `repo+0x90`.
- find `FUN_140d63c30` `er+0xd63c30`; lookup `FUN_140d62c10` `er+0xd62c10`; twin `FUN_140d63e50`
  `er+0xd63e50` (map `repo+0xb0`, lookup `FUN_140d62ce0` `er+0xd62ce0`).
- node: `_Left+0x00 _Parent+0x08 _Right+0x10 _Isnil(u8)+0x19`; key DLWString `+0x28` (len `+0x38`,
  cap `+0x40`, heap iff cap>=8); value `CSTextureImage* +0x50`.
- image: vtable `er+0x2BB8910`; rect `+0x74..0x80`; `+0x10→Render::Texture+0x70→ID3D12Resource`;
  format offset UNIDENTIFIED (sheet+0x30 reads 0 live — open sub-task).
- clean global caller `FUN_140d7c940` `er+0xd7c940`. enumerate (load-screen-scoped) `FUN_140d69640`
  `er+0xd69640`.
- deliverables: `tools/cheat_engine/MapForGoblins_icon_repo.CT`, `D:\ghidra_scripts\walk_icon_repo.py`.
