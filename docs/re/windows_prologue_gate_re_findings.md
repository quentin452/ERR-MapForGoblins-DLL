# RE findings — "in prologue" boolean to gate the ImGui map (Chapel of Anticipation)

Goal: gate the overlay world map during the **prologue (Chapel of Anticipation)**, where the
player is not on the Lands Between map and the marker projection is invalid. Source: ERR EMEVD
decompiled to JS at `D:\tools\emevd_js\err\` (DarkScript3 batch mode). App 2.6.2.0 / ERR 2.2.9.6.

---

## TL;DR — the boolean is **event flag `120`**

- **`flag 120 OFF` ⇒ still in the prologue** (Chapel of Anticipation, map `m19_00_00_00`).
- **`flag 120 ON` ⇒ prologue finished.**

So `in_prologue = !goblin::ui::read_event_flag(120)`. No new RE needed — `read_event_flag`
(`goblin::ui::read_event_flag(uint32_t)` → `orp_flag_set`, `IS_EVENT_FLAG` AOB in
`re_signatures.hpp`) already exists.

---

## Evidence (EMEVD `m19_00_00_00` = the Chapel of Anticipation)

`m19_00_00_00` is the prologue map: its events cutscene-warp the player out to area `60096`
(the Stranded Graveyard exit) and set the low global progression flags.

- Flag 120 is set **`ON` only on leaving the Chapel**, in the two exit events `19000110` /
  `19000120`, immediately before the respawn-point + save:
  ```js
  SetEventFlagID(120, ON);
  SetEventFlagID(6010, ON);
  AwardAchievement(2 /*or 3*/);
  SetPlayerRespawnPoint(11102020);   // Stranded Graveyard / Cave of Knowledge
  SaveRequest();
  SetEventFlagID(21 /*or 22*/, ON);  // which prologue path was taken
  ```
- Every prologue event in `m19` is guarded by **`EndIf(EventFlag(120))`** (lines 161, 214,
  246, …) — i.e. "if 120 is already ON, the prologue is done, do nothing". This is the
  engine's own "am I past the prologue" test.
- Other maps gate on it too (e.g. `m11_10` `EndIf(!EventFlag(120))` / `WaitFor(EventFlag(120))`).

Robust in **NG+**: the Chapel is not replayed, flag 120 stays ON ⇒ never gated post-prologue.

---

## Wiring (recommended)

The overlay map shows via `g_show` (F1 toggle), **not** `world_map_open()` (the CSMenuMan+0xCD
auto-show signal is dead on 2.6.2.0). Gate at `goblin_overlay.cpp`, right after
`g_show = g_user_show;`:

```cpp
g_show = g_user_show;
// Gate the overlay map during the Chapel of Anticipation prologue. Flag 120 is set ON only
// when leaving the Chapel (m19 EMEVD), so !flag(120) == still in the prologue, where the
// player isn't on the Lands Between map and the projection is invalid.
if (g_show && !goblin::ui::read_event_flag(120))
    g_show = false;
```

---

## Caveat / alternative

Flag 120 gates the **prologue** only. It does **not** cover the optional later Chapel visit
(via the Four Belfries / Imbued Sword Key) — flag 120 is already ON there. To gate **all**
Chapel visits, test the **current map id == `m19_00_00_00`** instead (needs a current-map-id
reader; reuse the WorldChrMan chain in `docs/re/re_findings_playerpos.md` /
`D:\ghidra_proj2`). For "the prologue", flag 120 is the exact, canonical boolean.
