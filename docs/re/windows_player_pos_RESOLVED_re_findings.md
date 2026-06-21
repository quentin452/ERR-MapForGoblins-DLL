# RE findings — player position RESOLVED (runtime-confirmed chain)

**Status: SOLVED, runtime-confirmed** (Cheat Engine "find what accesses" + live RPM walk, not a
static guess). App 2.6.2.0 / ERR 2.2.9.6. Resolves
`docs/re/windows_player_pos_runtime_ct_re_prompt.md` and supersedes the static-derived player-pos
docs (`windows_underground_player_pos_re_findings.md`, `windows_yellowdot_player_pos_re_findings.md`).

---

## 0. The chain

```
WorldChrMan = [eldenring.exe + 0x3D65F88]      ; AOB WCM_FINDER (already PASS in re_signatures.hpp)
LocalPlayer = [WorldChrMan + 0x1E508]
X = float [LocalPlayer + 0x6C0]
Y = float [LocalPlayer + 0x6C4]                ; height
Z = float [LocalPlayer + 0x6C8]
```
CE pointer entry: base `eldenring.exe+3D65F88`, offsets `0x1E508` → `0x6C0` (X) / `0x6C4` (Y) /
`0x6C8` (Z). Module-anchored → survives restart. **Correct on every page, underground included**, and
tracks movement (value-scanned live at Ancestral Woods/Siofra = 1444.26 / -815.14 / 1542.22).

> A 2nd copy of the vector sits at `LocalPlayer+0x6D4/0x6D8/0x6DC` (render/prev copy). Use `+0x6C0`.

---

## 1. How it was confirmed (so it's not another static guess)

- CE **"find out what accesses"** the value gave two writers, both with **`RBX = 0x18224665080`**:
  - `eldenring.exe+0x659F3D`: `movsd [rbx+0x6C0]; mov [rbx+0x6C8],eax`  → X,Y then Z (copy 1).
  - `eldenring.exe+0x65A3BD`: `movsd [rbx+0x6D4]; mov [rbx+0x6DC],eax`  → copy 2.
- Live RPM walk: `WorldChrMan = [base+0x3D65F88] = 0x18224690080`;
  `[WorldChrMan+0x1E508] = 0x18224665080` = **exactly `RBX`** = the struct holding the position.
  ⇒ `RBX` is the **LocalPlayer**, position at `+0x6C0`.
- Module loaded (this session, ASLR) at `0x7FF61AB80000` — resolve the real module base at runtime,
  do NOT assume `0x140000000`. RVAs above are stable; the base is not.

## 2. Why the old probes failed (corrected)

- `get_player_world_pos` used `WorldChrMan + 0x10EF8` (drifted/dead) `→ [+0] → +0x6B0`. Correct is
  `WorldChrMan + 0x1E508` then **`+0x6C0` directly** (no extra `[+0]`; field moved `+0x6B0`→`+0x6C0`).
- `get_player_map_pos` read the geom manager `+0x70/+0x74` (block-local) + the MapId coarse tile,
  which is the wrong frame underground. The real player position is the ChrIns field above.
- Earlier static "chain A" (`+0x58→+0x10→+0x190→+0x68→obj+0x70`) resolves to a *different*
  sub-area-local transform (read 9.8/0.8/0.4) — it was an over-deref, not the world position.

## 3. Mod change (`goblin_inject.cpp`)

Replace the `get_player_world_pos` chain with:
```cpp
uint8_t *wcm = *(uint8_t**)(er_base + 0x3D65F88);            // WCM_FINDER
uint8_t *lp  = wcm ? *(uint8_t**)(wcm + 0x1E508) : nullptr;  // LocalPlayer
if (lp) { x = *(float*)(lp+0x6C0); y = *(float*)(lp+0x6C4); z = *(float*)(lp+0x6C8); }
```
All reads via ReadProcessMemory/SEH-guarded as today. `er_base = GetModuleHandle("eldenring.exe")`
(handles ASLR). This single source serves **both** consumers: distance-adaptive (map open) AND the
future minimap (map closed) — the ChrIns field updates every frame regardless of the map dialog.

## 4. The frame — SOLVED: `+0x6C0` IS `WorldMapPointParam.posX/posZ`

`LocalPlayer+0x6C0`(X)/`+0x6C8`(Z) is the player's position **in the exact `posX/posZ` param frame**
markers use. The world coord is then the SAME bridge as markers:
```
worldX = MapId_gridX*256 + [LocalPlayer+0x6C0]
worldZ = MapId_gridZ*256 + [LocalPlayer+0x6C8]
```
(MapId tile from the singleton `+0x2c`; then `project_dungeon_row_to_overworld` /
`marker_world_pos(area, gridX, gridZ, posX, posZ, conv_underground=…)` exactly like a marker.)

**Runtime validation — standing ON the "Bois des Fidèles" grace (area 12 underground):**
| | grace anchor (baked) | live `LocalPlayer+0x6C0` |
|---|---|---|
| posX | 1438.0 | 1437.823 |
| posZ | 1519.0 | 1519.030 |
| MapId gridX/gridZ | 2 / 0 | 2 / 0 |
→ `worldX = 2*256 + 1437.8 = 1949.8 ≈ wx 1950`; `worldZ = 0 + 1519.0 = wz 1519`. Exact match. The
overworld reading (tile 46,40; `+0x6C0` ≈ -69.7 / 3.8 → small = a `posX/posZ`) is consistent — same
frame on every page.

**Why the old underground path failed (root cause, confirmed):** `get_player_map_pos` read the local
from the **geom manager `+0x70/+0x74`** (a *physics-block* frame, e.g. 5.8) instead of
`LocalPlayer+0x6C0` (the *param* frame, 1437.8). The two differ by the physics-block origin (a
constant `+1432 / -816 / +1544` for the m12_02_00 block) — that delta is the wrong-underground bug.
The MapId tile + `tile*256 + local` + projection were all correct; only the local *source* was wrong.

### Fix (`goblin_inject.cpp::get_player_map_pos`)
Replace the local source — keep everything else (MapId tile, `gx*256+lx`, projection):
```cpp
uint8_t* wcm = *(uint8_t**)(er_base + 0x3D65F88);                 // WCM_FINDER
uint8_t* lp  = wcm ? *(uint8_t**)(wcm + 0x1E508) : nullptr;       // LocalPlayer
if (lp) { pr.lx = *(float*)(lp + 0x6C0); pr.lz = *(float*)(lp + 0x6C8); }  // posX/posZ (was geomMgr+0x70/+0x74)
```
Works overworld AND underground (m12; DLC m40-43/61 via the same conv), map-open AND map-closed
(minimap) — one source. SEH/RPM-guard as today.

## 5. Offsets / AOBs (version-stability)
- WorldChrMan `eldenring.exe+0x3D65F88` — `WCM_FINDER` `48 8B FA 0F 11 41 70 48 8B 05` (already in
  `re_signatures.hpp`, rel `{{0xA,0xE}}`).
- LocalPlayer = `[WorldChrMan + 0x1E508]`; player pos `LocalPlayer + 0x6C0`(X)/`+0x6C4`(Y)/`+0x6C8`(Z)
  (2nd copy `+0x6D4/0x6D8/0x6DC`).
- Writer fns (if a hook is ever wanted, not needed for the read): RVA `0x659F3D` / `0x65A3BD`.
- Tooling (this session): `D:\ghidra_scripts\find_chain.py` (RPM chain test + reverse scan); module
  base resolved live via Toolhelp (ASLR — was `0x7FF61AB80000`).
