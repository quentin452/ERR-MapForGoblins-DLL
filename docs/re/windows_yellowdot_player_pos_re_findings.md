# RE findings — yellow-dot player position: the Vec axis bug + the real source question

Static Ghidra (`D:\ghidra_proj2\ER`, script `find_yellowdot.java`) for
`docs/re/windows_player_pos_runtime_ct_re_prompt.md` (commit 6a68c1e). App 2.6.2.0 / ERR 2.2.9.6.

> **Static-only — VALIDATE in the CT before trusting** (static gave wrong offsets twice here; see
> `player-pos-static-unreliable`). But this reframes the brief's "origin underground" conclusion
> with a concrete, falsifiable hypothesis.

---

## 0. TL;DR — two concrete bugs, and the source may already be there

`FUN_14045e390` (the Vec getter at the end of the player-pos chain) and `FUN_1406614d0` are
**identical** and definitive:
```c
out[0] = *(qword*)(obj + 0x70);   // X @ +0x70, Y @ +0x74   (two floats in one qword)
out[1] = *(qword*)(obj + 0x78);   // Z @ +0x78, W @ +0x7c
```
So the position Vec is **X@+0x70, Y@+0x74 (HEIGHT), Z@+0x78**. Therefore:

1. **The code reads the wrong axis.** `get_player_map_pos` reads `+0x70`(X) and **`+0x74`** as "Z" —
   but `+0x74` is **Y (height)**. Z is **`+0x78`**. This cleanly explains overworld-works /
   underground-fails:
   - Overworld (test #1: `+70=-7.7 +74=3.3 +78=0.5`): flat ground → Y≈3.3 is small → using it for Z
     is a tiny error at 256-units/tile scale → "lands on the dot".
   - Underground (test #1: `+70=1.5 +74=-5.3 +78=5.4`): Siofra is **deep** → Y (height) swings hard
     → using Y as Z throws the dot far off. **The values are NOT origin — they're small because
     they're sub-area-local** (near Siofra's local frame origin).
2. **The runtime `(0,0,0)` (test #2) was a short read.** The chain leaf is `*(c+0x68)` **then
   `+0x70`** (via `FUN_14045e390`), not the Vec *at* `c+0x68`. Test #2 read a pointer's bytes as
   floats → garbage/zero. The corrected leaf is `[[[[ *(WCM+0x1e508) +0x58]+0x10]+0x190]+0x68]+0x70`.

**Reframed conclusion:** the manager/chain almost certainly *does* hold the underground player
position — as **sub-area-local** coords at `+0x70`(X)/`+0x78`(Z). The brief read them as "≈origin"
because (a) they're small (local frame) and (b) `+0x74` (height) was taken for Z. The real problems
are the **axis** and the **underground bridge** (next), not a missing source.

---

## 1. The decisive runtime check (do this first — one CT/log, ~2 min)

At an underground grace (Ancestral Woods), read `mgr+0x70`, `+0x74`, `+0x78` **separately** and move:
- Walk N/S/E/W → **two** of them change = the horizontal axes (expect `+0x70`=X, `+0x78`=Z).
- Jump / go up/down a slope → the **third** changes = Y height (expect `+0x74`).

If `+0x70`/`+0x78` track horizontal movement underground → **the source is correct**, and the only
fixes are the axis (`+0x74`→`+0x78`) and the bridge (§2). If none track movement → the source really
is elsewhere; fall through to the cursor pipeline (§3).

`mgr` = `er_base + 0x3d69ba8` (the same manager `get_player_map_pos` already reads via
`WORLD_GEOM_MAN_SLOT`). Or read the raw chain leaf above.

---

## 2. The underground bridge (the second bug)

Overworld uses `world = tile*256 + local`. Underground sub-areas (m12 Siofra/Ainsel/Nokron, DLC
m40-43) are **not** a simple tile*256 layout — they project onto the underground map *page* via the
same conversion markers use. `get_player_map_pos` already calls `project_dungeon_row_to_overworld`,
but underground falls to the plain `tile*256 + local` else-branch. **Feed the player through the
exact path underground markers use** — `marker_world_pos(area, gridX, gridZ, X, Z,
conv_underground=true)` — with `X=+0x70`, `Z=+0x78`, and `gridX/gridZ` from MapId `+0x2c` (coarse but
correct underground). The player dot then lands wherever a marker at the same spot lands.

---

## 3. The actual yellow-dot pipeline (deeper lead, if §1 says the source is elsewhere)

The map cursor (`CS::WorldMapCursorControl`, ctor `FUN_1409bc5b0`, tick `FUN_1409bd4b0`) computes the
on-map player/cursor position via a **position provider** at `cursor+0x90`:
```c
src = (**(code**)(*(cursor+0x90) + 8))();        // provider → a world/area position
mapUV = FUN_140d82770(src, &out, &region);       // project world → map-image UV (per-area)
```
`FUN_140d82770` is the **world→map-UV projection** (per-area, correct underground by construction);
`cursor+0x90`'s provider is wired in the menu setup `FUN_1409be5e0` (RVA 0x9be5e0, ~9.5 KB). If §1
fails, RE `cursor+0x90`'s provider + `FUN_140d82770` — that is exactly what draws the dot, so its
input is the authoritative correct-everywhere player position. (Map-OPEN only; for the minimap you'd
still need the map-closed source, i.e. §1/§2.)

---

## 4. Recommendation

1. Run §1. Most likely outcome: `+0x70`/`+0x78` track movement → apply **axis fix `+0x74`→`+0x78`**
   + route underground through `marker_world_pos(conv_underground=true)` (§2). One CT/log confirms.
2. If the source is genuinely dead underground, RE the §3 cursor provider + `FUN_140d82770`.

This supersedes `windows_underground_player_pos_re_findings.md` (its `+0x74`=Z claim and the
`+0x1e508`→`+0x68` direct read were wrong; corrected here).

## 5. Offsets / AOBs
- Player pos chain leaf: `[[[[ *(WorldChrMan+0x1e508) +0x58]+0x10]+0x190]+0x68]` → object; pos at
  **`+0x70`(X) / `+0x74`(Y/height) / `+0x78`(Z)**. WorldChrMan = `er_base+0x3d65f88` (`WCM_FINDER`).
- Same Vec mirrored at manager `er_base+0x3d69ba8 + 0x70/0x74/0x78` (built by `FUN_1406d3a20`).
- Vec getter `FUN_14045e390` AOB `0F 10 41 70 48 8B C2 0F 29 02 C3` (`movups xmm0,[rcx+0x70];
  movaps [rdx],xmm0`) — note: only copies `+0x70..+0x7f` (16 bytes = X,Y,Z,W).
- MapId singleton `er_base+0x3d691d8`, packed id `+0x2c` (area`>>24`, gridX`>>16`, gridZ`>>8`).
- Cursor: `WorldMapCursorControl` ctor `FUN_1409bc5b0`, tick `FUN_1409bd4b0`; provider `cursor+0x90`;
  world→map projection `FUN_140d82770`; menu setup `FUN_1409be5e0`.

Script: `D:\ghidra_scripts\find_yellowdot.java` (output `out_yellowdot.txt`).
