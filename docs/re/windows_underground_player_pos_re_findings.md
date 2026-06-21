# RE findings — underground player position (m12 / DLC m40-43) for the overlay map

Answers `docs/re/windows_underground_player_pos_re_prompt.md`. Static Ghidra (`D:\ghidra_proj2\ER`,
scripts `find_ugpos.java` / `find_ugmgr.java`). App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`.
Resolve by AOB. Builds on `docs/re/re_findings_playerpos.md`.

> Underground *validity* is intrinsically runtime (the structures differ underground). The chain +
> offsets below are decompiler-confirmed; the live pick between the two fix candidates and the
> block-local↔marker bridge are a quick runtime check (recipe in §5).

---

## 0. TL;DR

The engine already computes the player's map position **the same way everywhere** (overworld +
underground). It is built by **`FUN_1406d3a20`** into the map-point manager **`DAT_143d69ba8`**
(`er_base + 0x3d69ba8`) at **`+0x70`(X) / `+0x74`(Y) / `+0x78`(Z)** (a 3D vec, two qwords), from:

```
player = *(WorldChrMan/*DAT_143d65f88*/ + 0x1e508)          // ← the LIVE LocalPlayer field
pos    = [[[ *(player + 0x58) + 0x10] + 0x190] + 0x68]      // player physics transform → Vec(X,Y,Z)
```

Two concrete bugs in the current probes follow:

1. **`get_player_world_pos` uses the STALE base `WorldChrMan + 0x10EF8`.** That field drifted; the
   live one is **`+0x1e508`** (same as the engine builder). Its "chain B"
   (`player+0x58 → +0x10 → +0x190 → +0x68`) is otherwise the correct chain — it returns null
   underground only because it starts from the dead `0x10EF8` player. **Fix: base `0x1e508`.**
2. **`get_player_map_pos` reads the wrong source/axis underground.** Read the engine's value at
   `DAT_143d69ba8 + 0x70`(X)/`+0x78`(Z) (note **Z is `+0x78`, not `+0x74`** = height), or replicate
   the `+0x1e508` chain directly, then bridge to marker space with the MapId tile.

The "~origin underground" symptom is explained: when `*(WCM+0x1e508)==0` the builder takes a
**fallback** that returns a **block anchor (origin)**, not the player (see §2).

---

## 1. The builder `FUN_1406d3a20(mgr, dt)` (RVA 0x6d3a20)

```c
// default-fill mgr+0x70..0x7c with DAT_144802260 (a zero/identity vec)
if (*(WorldChrMan + 0x1e508) != 0) {                       // MAIN path
    p = FUN_1403f0bf0(*(WorldChrMan + 0x1e508), &vec);     // = FUN_1403c6ff0(*(player+0x58))
    //   FUN_1403c6ff0(a): FUN_14045e390( [[a+0x10]+0x190]+0x68 )   → fills vec(X,Y,Z,W)
} else {                                                   // FALLBACK path
    FUN_140243a60(DAT_143d691d8, &mapKey);                 // current map-id key
    node = rbtree_lookup(mgr+0x20, mapKey);                // block tree
    p = FUN_1406614d0(node.block + 8, &vec);               // reads block_obj+0x70/+0x78 = ANCHOR
}
*(mgr + 0x70) = p[0];   // X@+0x70, Y@+0x74
*(mgr + 0x78) = p[1];   // Z@+0x78, W@+0x7c
// then mgr+0x80..0x8c = smoothed/camera copy (FUN_1403bb1c0) or copy of +0x70
```
- `FUN_1403c6ff0` (RVA 0x3c6ff0): `FUN_14045e390([[a+0x10]+0x190]+0x68)` — `+0x190` is the player
  physics module, `+0x68` its position; the `+0x1e508`→`+0x58` hops reach it. Same node the brief's
  "chain B" walks — only the **base offset** differs (`0x10EF8` dead vs `0x1e508` live).
- `FUN_1406614d0` (RVA 0x6614d0): `out[0]=obj+0x70; out[1]=obj+0x78` — the fallback's block-anchor
  copy. If the main field is null underground, this is the **origin** the brief sees.

## 2. The manager (confirmed) — `DAT_143d69ba8`

`FUN_1406d31f0` (RVA 0x6d31f0, the per-frame update) calls the builder then walks the map-point tree
at `mgr+0x20`, testing each point against **`mgr+0x70`** (player) / `mgr+0x80` (smoothed) via
`FUN_1406a92a0`. So `+0x70/+0x78` is the exact frame the map-point system compares in.

Its caller `FUN_140623410` loads the manager at `140623d94`:
`MOV RCX, [0x143d69ba8]` → **`DAT_143d69ba8`** = `er_base + 0x3d69ba8`. (The current code's
`WORLD_GEOM_MAN_SLOT` is annotated "was 0x3D69BA8" but is shared with `goblin_collected`'s
CSWorldGeomMan, whose `+0x70/+0x74` are a 2D block-local X/Z that works overworld and reads origin
underground — that mismatch is the second bug.)

## 3. Frame — block-local, bridge with the MapId tile

Per `re_findings_playerpos.md` (v19/v20: the path is a pure Vec copy, no arithmetic), the value is
the **raw block-local physics Vec**, not global marker space. Convert exactly like markers:
```
world_x = gridX * 256 + localX        // localX = pos.X
world_z = gridZ * 256 + localZ        // localZ = pos.Z   (pos.Z = vec +0x8, i.e. mgr+0x78)
```
`gridX/gridZ` come from the **MapId singleton `+0x2c`** (`DAT_143d691d8`), which is reliable
underground (the brief confirms the tile reads correctly; only the fine local was wrong).

---

## 4. Recommended fix

**Candidate A (robust — replicate the engine source).** Independent of which manager an AOB hits:
```c
uint8_t *wcm    = *(uint8_t**)(er_base + 0x3d65f88);          // WorldChrMan (WCM_FINDER)
uint8_t *player = *(uint8_t**)(wcm + 0x1e508);                // LIVE LocalPlayer (was 0x10EF8)
if (player) {
    uint8_t *a = *(uint8_t**)(player + 0x58);
    uint8_t *b = *(uint8_t**)(a + 0x10);
    uint8_t *c = *(uint8_t**)(b + 0x190);                     // physics module
    float *v   = (float*)(c + 0x68);                          // Vec: v[0]=X, v[1]=Y, v[2]=Z
    localX = v[0]; localZ = v[2];                             // horizontal = X,Z (NOT X,Y)
}
// then world = gridX*256 + localX, gridZ*256 + localZ  (gridX/Z from MapId +0x2c)
```
All hops SEH-guarded. This fixes both `get_player_world_pos` (stale base) and `get_player_map_pos`
(underground). Then drop the `!(open_grp & 1)` overworld-only gate in `map_renderer.cpp`.

**Candidate B (one-liner — test first if you keep the manager read).** If the live map-pos read is
actually `DAT_143d69ba8`, just change the Z axis `+0x74 → +0x78` (the current `+0x74` is height).
Re-test underground; if still origin, the manager took the fallback path → use Candidate A.

---

## 5. Runtime decision (quentin, at the Mimic Tear grace, area 12)

Log all of these once and compare to the known location:
1. `wcm`, `*(wcm+0x1e508)` (is the live player field non-null underground?), and Candidate-A
   `v[0]/v[1]/v[2]`.
2. `DAT_143d69ba8 + 0x70/0x74/0x78` (the engine's stored value) and `+0x80/0x88` (smoothed).
3. MapId `+0x2c` → area/gridX/gridZ.

- If `*(wcm+0x1e508)` non-null and `v` matches the real spot → **Candidate A**, with bridge
  `gridX*256 + v[0]`, `gridZ*256 + v[2]`.
- If `DAT_143d69ba8+0x70/+0x78` already matches → **Candidate B** (manager read, `+0x78` for Z).
- If both read origin → `*(wcm+0x1e508)` is null underground and the engine itself uses the
  fallback; then the only fine source is a deeper physics node — re-RE from `c+0x68` underground.

---

## 6. AOBs / offsets — version-stability

- WorldChrMan `DAT_143d65f88` = `er_base+0x3d65f88` (AOB `WCM_FINDER` `48 8B FA 0F 11 41 70 48 8B 05`).
- **LocalPlayer field `WorldChrMan + 0x1e508`** (replaces dead `+0x10EF8`).
- Player physics Vec: `[[[ *(player+0x58) +0x10] +0x190] +0x68]` → `X@+0, Y@+4, Z@+8`.
- Map-point manager `DAT_143d69ba8` = `er_base+0x3d69ba8`; player pos `+0x70`(X)/`+0x74`(Y)/`+0x78`(Z),
  smoothed `+0x80/0x84/0x88`; point tree `+0x20`.
- MapId singleton `DAT_143d691d8`; packed id at `+0x2c` (area `>>24`, gridX `>>16`, gridZ `>>8`).
- builder `FUN_1406d3a20` AOB `48 89 5C 24 18 57 48 81 EC A0 00 00 00 0F 29 B4 24 90 00 00 00 48 8B 05`;
  update `FUN_1406d31f0` AOB `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E9 48 81 EC E8`;
  source `FUN_1403c6ff0` AOB `40 53 48 83 EC 20 48 8B 41 10 48 8B DA 48 8B 88 90 01 00 00`.

Scripts: `D:\ghidra_scripts\find_ugpos.java`, `find_ugmgr.java` (outputs `out_ugpos.txt`,
`out_ugmgr.txt`).
