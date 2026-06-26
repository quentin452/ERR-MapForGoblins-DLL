# RE brief — DRIVE the world-map page switch (auto cross-page "locate"), safely

**Goal:** let the overlay PROGRAMMATICALLY switch the world-map page
(overworld ⇄ base-underground ⇄ DLC / Land of Shadow) so the F1 item-search "locate" can jump to a
result on another page in one click — instead of today's "switch to that page first, then it centres"
(the persistent-locate workaround, commit 6cef7ae). TWO hard constraints:

1. **Never switch to a page the player hasn't unlocked/discovered** (no DLC tab before owning SOTE +
   entering the Land of Shadow; no underground map before reaching it). A blind switch to a locked
   page is a spoiler / softlock risk.
2. **Gray out** search results whose page (or region) the player can't reach yet, and disable their
   locate, so the UI never offers an impossible jump.

App 2.6.2.0 / ERR 2.2.9.6, DLL in-process. Resolve as `[er_base+RVA]+offsets` / AOBs, runtime-verified.
The trigger is a WRITE/CALL path (not read-only) — so safety (thread, availability gate) is the crux.

## What we already resolve / know (don't re-derive)
- **WorldMapDialog** = `cursor − 0x2DB0`; cursor via the CSMenuMan walk (`resolve_cursor_via_menu`,
  goblin_worldmap_probe.cpp). WorldMapArea (view) = `cursor + 0xF0`.
- Open-page reads (probe `get_live_view` → `LiveView`): `openDlc` ← `dialog+0xA88` (page id; 10 = DLC),
  `underground` ← `[dialog+0x2B68]+0xB8` (layer byte: 0 surface / 1 UG), `viewArea` ← `WorldMapArea+0x6E`.
- **The page-switch HANDLERS are already decompiled** (docs/re/windows_worldmap_page_transition_re_findings.md
  §1/§6), but only as low-level leaves — not yet a safe, callable entry:
  - surface↔UG: `FUN_1409c40f0(dialog, layer, force)` @ RVA `0x9c40f0` (writes `[+0x2B68]+0xB8=layer`,
    calls page-apply, sets swap edge `+0xA44`).
  - base↔DLC: SIX siblings `FUN_1409c5d20 / c7900 / c1fc0 / c23d0 / c2c00 / c3280` — **which one does
    base→DLC vs DLC→base (and what the other four do) is UNPINNED.**
  - page-apply `FUN_1409c8120(dialog, page)` @ `0x9c8120` (writes the 9 `WorldMapItemControl` lists).
  - per-frame UI step `FUN_1409c32f0` @ `0x9c32f0` runs the transition; the handlers are normally called
    from the dialog's INPUT handler on the game/UI thread.
- **Fog / discovery oracle (already SOLVED, reusable):** `FUN_140886560(VM, layer, tileId) → flags`,
  a tile is revealed iff `(flags & 0x17fff)==0` (layer 0 = DLC); sorted table at `VM+0x288`;
  `tileId = group*10000 + gx*100 + gz` (memory: worldmap-unsearched-fog-mask). Per-tile, not per-page.
- Per-marker map fragment gate: `m.fragment_flag` (GetMapFlagFromTile), read live.

## The blocker we're solving
Calling `FUN_1409c40f0` (or a DLC sibling) directly from our **Present/render-thread** hook RACES the
game's per-frame UI step `FUN_1409c32f0` (same dialog/9-list state) → corruption / crash. And we have
no "is this page even reachable" gate, so a naive switch could jump to a locked DLC/UG page.

## What we NEED (decompile + runtime-confirm)
1. **A SAFE switch trigger to a target page.**
   - **Disambiguate the 6 DLC siblings.** Instrument: hook all 7 handlers (the 6 + `FUN_1409c40f0`),
     log `{which fn, args, resulting openDlc/underground}` while the user does
     overworld→UG→overworld→DLC→overworld. Output: the exact fn for each transition + its args
     (layer value, the `force` flag semantics, return).
   - **Threading / safe entry — THE key question.** Is there a HIGHER-LEVEL entry that is thread-safe
     or defers to the UI thread? Three candidates to evaluate, in order of preference:
     (a) the dialog's **input handler** that consumes the page-switch button/D-pad and *calls* these
         handlers — can we feed it a synthetic "switch to page N" event / set a request field it drains
         next UI step (the clean, in-thread path)? Find that handler + its input/request field.
     (b) a **queued command** the menu system already drains on the UI thread (set a pending-page field
         on the dialog that `FUN_1409c32f0` reads).
     (c) if neither exists, the **game-thread marshal**: which existing per-frame game-thread hook can
         we piggyback to run the call in-thread (not the render thread)? Is `FUN_1409c40f0` reentrant /
         safe to call once per frame from there?
   - Confirm whether the switch must be a no-op when already on the target page (idempotency).
2. **A "page AVAILABLE / discovered" predicate** (so we never switch to a locked page).
   - **DLC page:** the field/flag that makes the DLC map tab SELECTABLE — i.e. the same state the
     native map uses to enable/disable the DLC page button (owns SOTE + has entered the Land of Shadow).
     Prefer the UI's own "tab enabled" boolean over guessing an event flag. RVA/offset off the dialog
     (likely near the 9 list objects / a tab-control), or the gating event flag id.
   - **Underground page:** likewise — the state that enables the underground-map toggle (player has
     reached an underground region). Offset/flag.
   - Net: a `page_selectable(group)` read the game already trusts for its own button enable.
   - **Per-region (finer, optional but matches the constraint):** even on an available page, a specific
     marker's tile may be unrevealed. Reuse the fog oracle above (`FUN_140886560` / the `VM+0x288`
     table) to ask "is the marker's tile revealed?" — so we can gray a result that's on an unexplored
     part of an otherwise-open page.
3. **(graying)** With (2), the overlay grays + disables a result whose page is `!page_selectable` (and,
   optionally, whose tile is unrevealed). No RE beyond (2) — just confirm the predicate is cheap enough
   to evaluate per result at search time (cache per page; fog per tile only on demand).

## Leads
- Start from the INPUT handler: find the caller(s) of `FUN_1409c40f0` / the DLC siblings (xrefs) — that
  caller reads the button/event and is the natural safe entry. Its "which page" argument or a request
  field is what we want to set instead of calling the leaf.
- Page availability ≈ the map's tab/button enable state. The WorldMapDialog tab control (near `+0xA88`
  / the 9 lists) likely holds per-page enabled bytes; or the DLC tab is gated by an event flag set on
  first DLC-map open. Cross-check against a save WITHOUT DLC entered (tab absent/disabled) vs WITH.
- Underground availability may be implicit (the toggle only appears underground) — confirm whether the
  overworld map even offers an "underground" target, or if UG is only reachable while physically there.
- `FUN_1409c32f0` reads the dialog every UI frame — a pending-request field it drains would be the
  cleanest marshal target (candidate (b)).

## Deliverable
- The SAFE switch entry: fn/AOB + signature + the THREAD/marshal recipe (input-event, queued field, or
  game-thread hook), runtime-confirmed — switching overworld↔UG↔DLC from the DLL lands on the right
  page with NO crash across repeated swaps.
- `bool page_selectable(int group)` (RVA/offset chain or event-flag id), runtime-confirmed on saves
  that HAVE vs HAVEN'T unlocked DLC + underground (false before, true after).
- A tiny C++ snippet: `bool page_selectable(group)` + `void request_switch_to_page(group)` (no-op if
  unavailable or already there), both safe to call from the overlay.

## Plan once answered
Wire into the existing persistent locate (map_renderer `s_locate_nameid` / `locate_pending`,
goblin_overlay item-search panel): when a clicked result's marker is on a DIFFERENT but
`page_selectable` page, call `request_switch_to_page(targetGroup)`; the existing locate then pans the
instant that page opens (no user action). Results on `!page_selectable` pages render grayed + their
Selectable disabled (the click is a no-op). The persistent-locate fallback stays for any case the
switch can't satisfy. Never switch to a page the predicate rejects.

## Why
Removes the last manual step in cross-page item search (the user's "changer de page l'utilisateur vers
le bon endroit"), while the availability predicate keeps it spoiler-safe — exactly the constraint
"éviter de téléporter dans une page que le joueur n'a pas encore découverte". The handlers are already
RE'd; this brief is about the SAFE way to fire them (thread + gate), not finding them.
