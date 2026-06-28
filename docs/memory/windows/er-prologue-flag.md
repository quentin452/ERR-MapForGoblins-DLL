---
name: er-prologue-flag
description: ER event flag 120 = prologue (Chapel of Anticipation) done; use to gate the ImGui map
metadata: 
  node_type: memory
  type: reference
---

**Event flag `120`** is ER's "prologue complete" boolean. `OFF` ⇒ player is still in the
prologue **Chapel of Anticipation** (`m19_00_00_00`); `ON` ⇒ prologue finished. Set `ON` only
on leaving the Chapel (ERR EMEVD `m19_00_00_00`, events `19000110`/`19000120`, right before
`SetPlayerRespawnPoint(11102020)` + `SaveRequest()`); every prologue event is guarded by
`EndIf(EventFlag(120))`. Stays ON in NG+ (Chapel not replayed).

To gate the ImGui world map during the prologue: in [goblin_overlay.cpp] after `g_show =
g_user_show;`, add `if (g_show && !goblin::ui::read_event_flag(120)) g_show = false;`. The map
shows via `g_show` (F1 toggle), not `world_map_open()` (CSMenuMan+0xCD is dead on 2.6.2.0).

Caveat: flag 120 does NOT cover the optional later Chapel visit (Four Belfries) — 120 is
already ON there. To gate ALL Chapel visits, test current-map-id == `m19_00_00_00` instead
(needs a current-map-id reader). EMEVD source decompiled at `D:\tools\emevd_js\err\` — see
[[darkscript3-emevd-decompile]]. Reuse [[ghidra-worldmap-re]] for any map-id RE.
