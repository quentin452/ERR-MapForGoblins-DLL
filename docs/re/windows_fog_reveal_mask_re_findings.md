# RE findings — map-fragment FOG-OF-WAR reveal state (gate the overlay markers)

Answers `docs/re/windows_fog_reveal_mask_re_prompt.md`. Static Ghidra (`D:\ghidra_proj2\ER`,
scripts `find_fogmask.java` / `find_fogmask2.java`, headless `-process`) + Paramdex cross-ref.
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Resolve by AOB.

---

## 0. TL;DR — **Path A is the answer, and it's pure data**

The reveal state is a **per-piece event flag**, and the tile→piece mapping is a **point-in-
rectangle test in map-space**. No VMP code, no engine call needed.

- A `WorldMapPieceParam` piece is **revealed ⇔ `IsEventFlag(openEventFlagId)` is true**
  (`openEventFlagId` = `row+0x4`; if it is `0`, the piece is "always shown" — no gate).
- The piece **covers the rectangle** `openTravelArea Left/Right/Top/Bottom`
  (`row+0x8/0xc/0x10/0x14`, f32, map-space). A marker at map-space (x,y) belongs to the piece
  whose rectangle contains it.
- Row id scheme: **`rowId = areaIdx*100 + pieceIdx`**, `pieceIdx ∈ 0..31`, mask bit = `rowId % 100`.
  `areaIdx ∈ {0, 1, 10}` (the three map layers; see §3).

⇒ Mod-side: read `WorldMapPieceParam` live (same `from::params` path as `WorldMapPointParam`), build
`piece = {rect(L,R,T,B), openEventFlagId}`. In `map_renderer.cpp`, when `requireMapFragments` is on,
skip a marker whose containing piece has `openEventFlagId != 0 && !read_event_flag(openEventFlagId)`.
This **replaces** the coarse `MapList`/`GetMapFlagFromTile` table with the engine's true fog state.

The brief's two open assumptions are both resolved favourably: (Q-A1) the reveal field **is** a
direct event flag, and (Q-A2) the tile→piece is a **rectangle in the row**, not a hidden index
formula — so we never have to guess geometry.

---

## 1. `WORLD_MAP_PIECE_PARAM_ST` — engine-confirmed field offsets (Q-A1)

The offsets the engine actually reads (Ghidra) map 1:1 to the Paramdex names:

| off | type | name | engine use |
|---|---|---|---|
| `+0x04` | u32 | **`openEventFlagId`** | reveal test (`FUN_1405d1330`, §2). `-1`→treated as `0`. |
| `+0x08` | f32 | **`openTravelAreaLeft`** (Xmin) | rect, read by container `FUN_1408890b0` |
| `+0x0c` | f32 | **`openTravelAreaRight`** (Xmax) | rect |
| `+0x10` | f32 | **`openTravelAreaTop`** (Ymin) | rect |
| `+0x14` | f32 | **`openTravelAreaBottom`** (Ymax) | rect |
| `+0x18` | u32 | `acquisitionEventFlagId` | acquisition cutscene (not reveal) |
| `+0x1c..0x30` | f32 | `acquisitionEvent*` | acquisition cutscene framing |

The lookup `FUN_140d56d90(wrapper, rowId)` fetches the `WorldMapPieceParam` table
(`FUN_140d4cc50(DAT_143d81ee8, 0x58, …)` — param category `0x58`), binary-searches the row id, and
returns the row data ptr in `wrapper+0x10` (and the row id in `wrapper+0x8`).

---

## 2. The reveal condition (Q-A3) — a plain event-flag test, no variation indirection

`FUN_1408882d0(toggleByte, areaIdx)` builds the 32-bit mask: for `i in 0..0x1f`, lookup row
`areaIdx*100+i`, and set bit `(rowId % 100 == i)` iff the piece is revealed. The reveal test is:

```c
bool FUN_1405d1330(mgr, int* pFlagId) {       // pFlagId = &row.openEventFlagId
    if (*pFlagId != 0) return FUN_1405f9400() != 0;   // FUN_1405f9400 = IsEventFlag(flagId)
    return false;                                     // flagId 0 ⇒ "not gated"
}
```
The `"…::_GetMapVariation"` / `"…::_IsOpenMapPiece"` strings passed alongside are **profiler scope
tags** — `FUN_1405d8200(buf, name)` just writes `{1, name}` into a stack scope object; there is **no
map-variation evaluation**. So the engine's reveal == `IsEventFlag(openEventFlagId)`.

> When the toggle byte `DAT_143d6cfc0` (NewMenuSystemWarp2) ≠ 0, `FUN_1408882d0` short-circuits to
> `0xffffffff` (all revealed) — the existing debug-reveal path; irrelevant to the gate.

**Q-A3 answer:** reading the **live** param's `openEventFlagId` and testing it with our existing
`goblin::ui::read_event_flag` is exactly what the engine does. ERR's flag remap is already baked into
the live regulation's `openEventFlagId` values and into the live flag manager, so both sides agree —
**no separate indirection to miss.** `openEventFlagId == 0` pieces (and tiles covered by no piece)
are simply ungated (always shown), which is correct.

---

## 3. Tile → piece (Q-A2, the crux) — a rectangle test

The container `FUN_1408890b0`, on each newly-revealed bit, reads the row geometry and places the
piece display object:
```c
rect = { row+0x8 /*L*/, row+0x10 /*T*/, row+0xc /*R*/, row+0x14 /*B*/ };
FUN_140888c80(mgr, areaIdx, &rect);     // guards: (R-L > 0) || (B-T > 0)  → non-empty rect
```
`FUN_140888c80` treats it as the corners `(Left,Top)`–`(Right,Bottom)` of a map-space rectangle (the
"踏破エリア" / explored-area region the fog overlay reveals). So:

> **A marker at map-space (x, y) is covered by the piece whose
> `openTravelAreaLeft ≤ x ≤ openTravelAreaRight && openTravelAreaTop ≤ y ≤ openTravelAreaBottom`.**

No per-tile table and no hidden index formula — the rectangle *is* the geometry. Build the marker→
piece test directly from these f32 fields.

**Row-id / areaIdx scheme** (`FUN_140887e00`, `FUN_1408890b0`):
- `rowId = areaIdx*100 + pieceIdx`, `pieceIdx ∈ 0..31`, mask bit = `rowId % 100`.
- The engine reconciles three layers: `DAT_142ad82f8 = {0x00, 0x01, 0x0A}` ⇒ **`areaIdx ∈ {0, 1, 10}`**,
  remapped to mask slots `{0, 1, 2}` (`param=0→0, 1→1, 10→2`). These are the **overworld / underground
  / DLC** map layers (Q-B2).

> Coordinate frame: `openTravelArea*` are in the world-map's internal overlay space (the same space
> `FUN_140884c50` rasterises pieces into, grid `0x28`=40, `<<8` cells). The mod already projects
> markers to map-space; calibrate once against a known piece (e.g. the West-Limgrave fragment's rect)
> to confirm the marker map-space ↔ `openTravelArea` transform per layer. This is the only runtime
> calibration step — the field semantics above are exact.

---

## 4. Recommendation — Path A (pure param read). Path B is the fallback.

### Path A (recommended) — read `WorldMapPieceParam` + per-piece event flag
Safe, no VMP, no engine call. Recipe:
1. Enumerate `WorldMapPieceParam` rows (live, `from::params`). For each row capture
   `{ id, L=+0x8, R=+0xc, T=+0x10, B=+0x14, flag=+0x4 }`. Group by layer via `id/100` (`areaIdx`).
2. Per marker: find the piece whose rect contains the marker's map-space point (within its layer).
3. Gate: hide the marker iff `flag != 0 && !read_event_flag(flag)`. (No covering piece, or
   `flag==0` ⇒ show — matches the engine.)

### Path B (fallback) — read the engine's computed mask
If live param-read is undesirable: the per-layer 32-bit revealed mask is cached at
**`[manager + 0x39c + slot*4]`** (`slot ∈ {0,1,2}` for `areaIdx {0,1,10}`), updated each frame by
`FUN_1408890b0`. `manager` is the world-map dialog/overlay object (`param_1` of `FUN_1408890b0`;
caller chain `FUN_1407ee850 → FUN_140887870 → FUN_1408890b0`). Read-only; bit `pieceIdx` = revealed.
You still need the §3 rectangle (from the param) to map a tile→pieceIdx, so Path B saves nothing over
Path A while adding a fragile singleton-find — **prefer Path A.** (Do **not** call `FUN_1408882d0`
directly to avoid touching VMP-adjacent code; reading the cached mask is the only safe Path-B use.)

---

## 5. Mod-side integration sketch

```cpp
// build once after params load (alongside WorldMapPointParam):
struct FogPiece { int layer; float l, r, t, b; uint32_t flag; };
static std::vector<FogPiece> g_fog_pieces;   // from WorldMapPieceParam rows

bool marker_fogged(int layer, float mx, float my) {           // mx,my = marker map-space
    if (!goblin::config::requireMapFragments) return false;
    for (const auto& p : g_fog_pieces) {
        if (p.layer != layer) continue;
        if (mx < p.l || mx > p.r || my < p.t || my > p.b) continue;   // not this piece
        if (p.flag == 0) return false;                                 // ungated piece
        return !goblin::ui::read_event_flag(p.flag);                   // fogged if flag unset
    }
    return false;   // covered by no piece ⇒ ungated (engine behaviour)
}
// map_renderer.cpp: skip marker m when marker_fogged(m.layer, m.mx, m.my).
```
This becomes the real backing for the `require_map_fragments` toggle; retire
`GetMapFlagFromTile`/`MapList` once calibrated.

---

## 6. AOBs / offsets — version-stability

| fn | role | AOB (entry) |
|---|---|---|
| `FUN_1408882d0` | mask builder (`toggleByte`, `areaIdx`) | `48 8B C4 56 57 41 54 41 56 41 57 48 83 EC 60 48 C7 40 A0 FE FF FF FF` |
| `FUN_1408890b0` | container / diff-reconcile (reads rect) | `48 8B C4 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 70 48 C7 45 B0 FE` |
| `FUN_140887e00` | `(areaIdx,pieceIdx)`→row (`*100+idx`) | `48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B D9 33 C0` |
| `FUN_1405d1330` | reveal test = `IsEventFlag(openEventFlagId)` | `48 83 EC 28 8B 12 85 D2 74 0F E8 C1 80 02 00 85 C0 0F 95 C0 48 83 C4 28` |
| `FUN_140d56d90` | param-row lookup (cat `0x58`) | `40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 6C 24` |

- `WORLD_MAP_PIECE_PARAM_ST`: `openEventFlagId`=`+0x4`; rect `openTravelArea L/R/T/B`=`+0x8/+0xc/+0x10/+0x14`.
- row id = `areaIdx*100 + pieceIdx`; mask bit = `rowId % 100`; layers `areaIdx ∈ {0,1,10}` →
  mask slots `{0,1,2}`; engine mask cache `[manager+0x39c + slot*4]`.
- toggle byte `DAT_143d6cfc0` (NewMenuSystemWarp2) ⇒ all-revealed when ≠0.
- event-flag reader: reuse `goblin::ui::read_event_flag` (`IS_EVENT_FLAG` AOB in `re_signatures.hpp`).
- We already have the flag reader; the only new data is the `WorldMapPieceParam` read (Path A).

Scripts: `D:\ghidra_scripts\find_fogmask.java`, `find_fogmask2.java`
(outputs `out_fogmask.txt` / `out_fogmask2.txt`).
