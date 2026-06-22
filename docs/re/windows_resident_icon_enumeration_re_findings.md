# RE findings — enumerate ALL resident item-icon sprites in one pass (FD4 image-repo walk)

Answers `docs/re/windows_resident_icon_enumeration_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v96`/`re_v97`, logs alongside). App 2.6.2.0 / ERR 2.2.9.6,
imagebase `0x140000000`. Read-only. Supersedes the §8 movie-walk in
`windows_runtime_item_sprite_re_findings.md` (that targeted load-screen movies on this build).

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
  sheet res + 0x30 = DXGI_FORMAT (vkd3d internal; 71=BC1_UNORM observed for the 4096x2048 atlases)
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
     sheet + `fmt=71`.
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

## 5. Secondary — force-load (still a lead, not solved)

The walk is resident-only. To cover items the player never browses, the sheet must be made resident
first. The not-found branch of the twin (`FUN_140d63e50`) shows the **create/insert** path
(`FUN_140d5fee0` build-node → `FUN_140d60d00`/`FUN_140d622f0` insert into the `repo+0xb0` map), i.e.
the in-repo registration — NOT a TPF sheet loader. A genuine force-load is the higher-level FD4
resource-load-by-name (load the `MENU_ItemIcon_<sheet>` gfx/TPF), which still needs the FD4 load entry
+ likely a live `GFxMovieView` (matches sprite-findings §3, `CSScaleformImageCreator::CreateImage`
`FUN_140d6bbc0`). Left as future RE — resident-only + browse-to-fill + baked PNG fallback is already
shippable.

---

## 6. Handles
- repo singleton `DAT_143d82510` `er+0x3D82510` (FD4Singleton); container map `repo+0x80`,
  `_Myhead` `repo+0x88`, `_Mysize` `repo+0x90`.
- find `FUN_140d63c30` `er+0xd63c30`; lookup `FUN_140d62c10` `er+0xd62c10`; twin `FUN_140d63e50`
  `er+0xd63e50` (map `repo+0xb0`, lookup `FUN_140d62ce0` `er+0xd62ce0`).
- node: `_Left+0x00 _Parent+0x08 _Right+0x10 _Isnil(u8)+0x19`; key DLWString `+0x28` (len `+0x38`,
  cap `+0x40`, heap iff cap>=8); value `CSTextureImage* +0x50`.
- image: vtable `er+0x2BB8910`; rect `+0x74..0x80`; `+0x10→Render::Texture+0x70→ID3D12Resource`;
  format `sheet+0x30`.
- clean global caller `FUN_140d7c940` `er+0xd7c940`. enumerate (load-screen-scoped) `FUN_140d69640`
  `er+0xd69640`.
- deliverables: `tools/cheat_engine/MapForGoblins_icon_repo.CT`, `D:\ghidra_scripts\walk_icon_repo.py`.
