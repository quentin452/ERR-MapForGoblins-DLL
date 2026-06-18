# RE findings — player world/map position (Target A)

Static Ghidra RE (project `D:\ghidra_proj2\ER.gpr`, Ghidra 12.1.2; headless scripts
`D:\ghidra_scripts\re_v13..v18.java`) for `docs/windows_re_briefing_playerpos_questbrowser.md`
**Target A** (a stable player position for proximity clustering v2). App **2.6.2.0**.

> **Confidence: STATIC-derived, NOT yet live-verified.** The chain + offsets below come
> from the decompiler; the exact manager singleton (3 candidates) and the "continuous
> across chunks" property must be confirmed live — recipe in
> `D:\DOWNLOAD\MapForGoblins_verify_playerpos_recipe.md`.

## Headline

The old CT chain is **dead on 2.6.2.0**: `LocalPlayerOffset 0x10EF8` now occurs **once** in
the whole binary (drifted). But the engine already computes a **player position in MAP /
MARKER space** (same units as `WorldMapPointParam.posX/posZ` and the map cursor) — so for
clustering we want *that*, and **no chunk→world bridge is needed**.

## The chain

- **WorldChrMan static:** RVA **`0x3d65f88`** (VA `0x143d65f88`). AOB
  `48 8B FA 0F 11 41 70 48 8B 05` `{{0xA, 0xE}}` (mov rax,[rip+disp] at +7; slot =
  finder + 0xE + disp32@+0xA). Resolved live, correct for 2.6.2.0 — **no drift**.
- **New player field:** `[WorldChrMan + 0x1e508]` (replaces the dead `+0x10EF8`).
- **Player MAP-pos builder `FUN_1406d3a20`** (RVA `0x6d3a20`): writes the player map position
  into a manager struct at **`+0x70` (X) / `+0x74` (Y) / `+0x78` (Z)** (+ a smoothed/camera
  copy at `+0x80/+0x84/+0x88`). Two source paths:
  - main: `FUN_1403f0bf0` → `FUN_1403c6ff0` reads `[[[WorldChrMan+0x1e508]+0x58]+0x10]+0x190 …]`
    and fills a Vec2;
  - fallback `FUN_1406614d0`: reads `obj+0x70`/`obj+0x78` directly.
  Either way the **position layout is `+0x70`=X, `+0x78`=Z** (Vec2, marker space).
- **Manager update `FUN_1406d31f0`** (RVA `0x6d31f0`, ← `FUN_140623410` RVA `0x623410`): per
  call it (1) builds the player pos at `manager+0x70/+0x78` via `FUN_1406d3a20`, then (2)
  walks a red-black tree of map points at `manager+0x20` and tests each against `manager+0x70`
  — i.e. this is the **world-map point / discovery manager**, and `manager+0x70/+0x78` is the
  live player position it uses.

## Live-readable handle (to verify)

`manager` is a world-map FD4Singleton; `FUN_140623410` updates several
(`DAT_143d692f8`, `DAT_143d69380`, `DAT_143d69ba8`, … RVAs `0x3d692f8` / `0x3d69380` /
`0x3d69ba8`). The one passed to `FUN_1406d31f0` is the map-point manager — **pin it live**:
read `[eldenring.exe + 0x3d69380] + 0x70/0x78` (try the 3 candidates), move the player, see
which changes. (`CSWorldMapPointMan` @ RVA `0x3d6e9d8` from the older RE is a 4th candidate.)

## Why this answers Target A

The briefing wanted a *stable, continuous* player position because the raw chains went
NaN / chunk-local. `manager+0x70/+0x78` is **already in marker space** (the projection the
map itself uses), so it is continuous across chunks by construction and directly comparable
to `WorldMapPointParam.posX/posZ` — proximity clustering can use it with **no calibration**.

## AOBs (entry; extend for uniqueness)

| RVA | function | AOB |
|---|---|---|
| 0x6d3a20 | player map-pos builder | (site AOB) `48 8B FA 0F 11 41 70 48 8B 05` is at RVA 0x6d3a4e inside it |
| 0x3f0bf0 | projector wrapper | `40 53 48 83 ec 20 48 8b 49 58 48 8b da e8` |
| 0x3c6ff0 | main-path pos reader | `40 53 48 83 ec 20 48 8b 41 10 48 8b 88 90 01 00 00 48 8b 49 68` |
| 0x6d31f0 | manager per-frame update | `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 e9 48 81 ec e8 00 00 00` |

Static globals: WorldChrMan `eldenring.exe+0x3d65f88`; player field `[WCM]+0x1e508`;
manager candidates `+0x3d692f8 / 0x3d69380 / 0x3d69ba8`; pos at `manager+0x70`(X)/`+0x78`(Z).

## Remaining (live, Cheat Engine — to run on Windows)
1. Pin the manager singleton (read `[+0x3d69380]+0x70/0x78` etc.; the one that tracks the
   player as you walk).
2. Confirm `+0x70/+0x78` == `WorldMapPointParam` space (compare to a nearby marker's posX/posZ).
3. **Walk across a map-block boundary (m60_XX_YY → adjacent)** → confirm `+0x70/+0x78` stays
   continuous (no reset/jump) — the property the old chains lacked.
4. Confirm it updates with the **map closed** too (clustering runs in-world, not just in-menu).
