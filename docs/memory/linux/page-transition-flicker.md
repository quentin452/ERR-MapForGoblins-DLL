---
name: page-transition-flicker
description: "Worldmap page-transition flicker — UG↔OW + OW↔DLC BOTH FIXED. OW↔DLC was NOT a position-jump/RE problem (old theory wrong): it was the view-delay 1-frame lag flashing the old map; reset() on group change fixed it (commit 3616a35)."
metadata: 
  node_type: memory
  type: project
---

Overlay marker flicker when switching worldmap pages. RE: `docs/re/windows_worldmap_page_transition_re_findings.md` (+ §7b runtime-confirmed 2026-06-22). LiveView exposes `swapEdge` (dialog+0xA44) + `fadeTimer` (dialog+0xE00) — commit 781378d on docs/worldmap-projection-re-brief.

**STATUS (2026-06-22): BOTH FIXED ✓ verified in-game.**
- **UG↔OW = FIXED** ✓ (same base-canvas, no pan move — set-swap fine, no lag needed).
- **OW↔DLC = FIXED** ✓ (commit 3616a35, branch docs/worldmap-projection-re-brief, DLL md5 90cea4).

**ROOT CAUSE (the OLD "position-jump / needs-RE" theory below was WRONG):** symptom = for exactly **1 frame** the OLD map shows on a OW↔DLC switch. Cause = the **motion-sync view-delay** (`g_view_delay`, ViewDelay ring buffer in goblin_projection.hpp, `kViewDelayFrames=1.0`). It delays the projected view 1 frame so markers ride the lagging native-map composite during smooth pans. But OW↔DLC the native canvas SNAPS instantly (page id `dialog+0xA88` + pan/zoom flip in the switch handler same frame) → our 1-frame-delayed view still projects the PREVIOUS (OW) canvas for one frame → old map flashes.
**FIX = trivial:** in map_renderer.cpp, compute `open_grp` BEFORE the delay; track `static int s_prev_grp`; on `open_grp != s_prev_grp` → `g_view_delay.reset()` (re-seeds the ring to the live, already-snapped view → that frame's delayed view == live → no stale flash). Same-page pans keep the 1-frame delay. No new RE, no fadeTimer/cross-fade needed.
**LESSON:** disambiguate the symptom first — "flicker" turned out to be a precise 1-frame stale-frame from our OWN delay buffer, NOT the engine's canvas animation. The whole "RE the eased base↔DLC view transform" plan was unnecessary. See [[disambiguate-bug-symptoms-first]].

**(stale theory, kept for context — DISPROVEN):** believed OW↔DLC was a position-jump needing the animated base↔DLC view-transform RE'd (`view+0x378` snaps; doc offsets `dialog+0x2EAC/+0x2EB0/+0x2EB4` read garbage). Tried delta-time page-LAG (commit 9d4e779) — failed, reverted. None of that was the real cause. `dialog+0xE00` fadeTimer (resets 0.2→0 over ~0.2s, sign=layer) + `swapEdge` (+0xA44) remain exposed on LiveView but are UNUSED by the fix.
