---
name: player-pos-static-unreliable
description: Static-decompiled player-pos offsets are unreliable on ER 2.6.2.0 — validate live (CT/yellow-dot)
metadata: 
  node_type: memory
  type: feedback
---

**RESOLVED (runtime-confirmed, CE find-what-accesses + RPM):** player world pos =
`[[eldenring.exe+0x3D65F88 (WCM_FINDER)]+0x1E508 (LocalPlayer)] + 0x6C0`(X)/`+0x6C4`(Y,height)/
`+0x6C8`(Z). Correct everywhere incl. underground, updates map-closed. (Old mod chain `WCM+0x10EF8→
[+0]→+0x6B0` = dead base; my static "chain A" `…+0x190+0x68+0x70` = a different sub-area-local
transform.) Resolve module base at runtime (ASLR), not 0x140000000. Frame SOLVED (validated on the "Bois des
Fidèles" grace): `+0x6C0`/`+0x6C8` ARE `WorldMapPointParam.posX/posZ`; `worldX = MapId_gridX*256 +
[+0x6C0]` (same bridge + conv as markers). The old `get_player_map_pos` bug = it read the local from
geomMgr+0x70 (physics-block frame) instead of LocalPlayer+0x6C0 (param frame). Fix = swap that
source. See `docs/re/windows_player_pos_RESOLVED_re_findings.md`.

Static Ghidra alone produced **wrong player-position offsets twice** on app 2.6.2.0 / ERR 2.2.9.6
(commit 6a68c1e refuted my `windows_underground_player_pos_re_findings.md`). Refuted at runtime:
- `*(WorldChrMan+0x1e508) → +0x58 → +0x10 → +0x190 → +0x68` Vec reads **(0,0,0) even overworld**
  (leaf/node wrong; the `FUN_1403c6ff0`/`FUN_14045e390` forwarders drop arg2 so the frame was
  *inferred*, not proven).
- `MAPPOINT_MGR_BUILDER` AOB is **ambiguous (2 matches)** and resolves to the **same slot** as
  `WORLD_GEOM_MAN_SLOT` (`er_base+0x3d69ba8`) — NOT a separate manager; `+0x70..+0x88` read origin
  underground (Ancestral Woods/Siofra tile 2,0). Overworld `+0x70/+0x74` bridged with MapId tile
  works; underground source is elsewhere.

**Correction (find_yellowdot):** the chain leaf is `*(c+0x68)` **then +0x70** (`FUN_14045e390`
copies `obj+0x70..+0x7f` = X@+0x70, Y@+0x74 HEIGHT, Z@+0x78). Test #2's (0,0,0) was a short read
(`c+0x68` is a pointer). The mod reads `+0x70`(X)/`+0x74`(Y) using HEIGHT as Z — masked overworld
(flat), broken underground (deep). Underground values are sub-area-local (small), NOT origin — the
source likely IS there. Real bugs = axis (`+0x74`→`+0x78`) + underground bridge (use
`marker_world_pos(conv_underground=true)`, not tile*256). Yellow-dot truth path = cursor+0x90 provider
→ `FUN_140d82770` world→map projection. See `docs/re/windows_yellowdot_player_pos_re_findings.md`.

**Why:** I over-trusted decompiler output for pointer-chain leaf offsets + manager identity.
**How to apply:** for live-memory chains (player pos, transforms), don't ship static-derived
offsets as "confirmed" — mark them "needs runtime confirm". The decisive validation is **Cheat
Engine vs the native yellow "you are here" dot** (correct on every page incl. underground). Better
lead than the physics chain = **the yellow-dot / worldmap-dialog player-marker DRAW PATH** (correct
by construction). <user> builds the DLL on Linux and can't run CE/the game; this box has the game
+ Ghidra ([[ghidra-worldmap-re]]) but live CE validation needs a human in the loop. Unified-frame
conv to reuse: `marker_world_pos` / `get_player_map_pos` (overworld output = reference).
