# RE brief — the ER IN-COMBAT state (gate the vmap like the native map)

**Why.** With the native-map REDIRECT shipped (`732fba1a`, `native_map_redirect_linux_re_plan.md`), pressing
the map key opens the MapForGoblins **vmap** and the native `WorldMapDialog` never constructs (we hook its
create-callback `FUN_1407fd4b0` er+0x7fd4b0). Vanilla ER **auto-disables the map in combat** (you can't open it,
and if it were open it closes). Our redirect breaks that: **ER blocks the map-open upstream in combat, so our
create-callback is never called → the vmap can't be toggled by the map key while in combat.** Result:
- Opening in combat is already prevented (ER's upstream block → our hook doesn't fire) ✓ — matches vanilla.
- **But a vmap already OPEN when combat starts is STUCK open** — the map key can't close it (create-callback
  blocked), and the player is left with a fullscreen map mid-fight. ✗

**Goal.** Find the ER **in-combat / danger state** MFG can read each frame, expose `combat_active()`, and gate
the redirect: while `vmap_redirect()` is set AND `combat_active()` → `set_vmap_redirect(false)` (force-close
the vmap). Optionally also hard-block open (belt-and-suspenders; ER already blocks it). This is the same state
the design docs need for the **fast-travel-in-combat** gate (the vmap's grace-warp must refuse in combat, like
ER), so one find serves both.

## Two ways in — do the LIVE one first (the game runs on this Linux box)
### Route 1 (preferred, Linux-live) — find the combat byte via mem_fwa / a known read
ER has a player combat/danger state (the "can't rest at a grace / can't warp / can't open map in combat"
gate). Candidates + how to pin live (the game is up via `tools/boot_hold.py`; `mfg.py rpc`):
1. **The map-open combat gate (cleanest — the EXACT check ER uses).** The create-callback `FUN_1407fd4b0`
   (er+0x7fd4b0) is dispatched from the menu-dialog factory table `er+0x2abb910`; SOMETHING upstream decides
   whether to open the map at all (it returns early in combat, before the table dispatch). `mem_fwa` won't
   catch a code path directly, so: in-world + NOT in combat, `mem_fwa` (read) on a scratch, then instead
   **hook or breakpoint the dispatch site** — OR simpler, diff-scan: dump the candidate combat flags below
   OUT of combat vs IN combat (aggro an enemy) and find the byte that flips.
2. **WorldChrMan → LocalPlayer combat/aggro byte.** `WCM_FINDER` AOB (pinned, `re_signatures.hpp`) resolves
   `CS::WorldChrMan`; the player is `WorldChrMan → PlayerIns` (chain already used by `windows_player_pos_RESOLVED`
   / `goblin_load_watchdog`). A combat/danger flag lives on PlayerIns or its ChrCtrl/AI module. **Method:** RPC
   `mem_dump` a window of PlayerIns fields out-of-combat, aggro an enemy, `mem_dump` again → the byte that goes
   0→1 (and back to 0 after ~seconds of no aggro) is the combat flag. Confirm it tracks the native map-block
   (open the native map with `vmap_on_map_key=0`: blocked exactly while the byte is 1).
3. **The "warp allowed?" gate.** ER forbids fast-travel in combat; the warp precondition (near `LUA_WARP` /
   the grace warp path, `windows_grace_warppin_teleport`) reads the same combat state. Trace the warp
   precondition read to the flag.

Deliverable: the flag's **resolve chain + offset** (e.g. `WorldChrMan(slot)→+0xXXXX→+0xYY` byte) live-verified
to flip on aggro and match ER's own map-block, plus a `combat_active()` in MFG (host) reading it SEH-guarded.

### Route 2 (Windows Ghidra fallback) — the map-open combat check
If the live diff-scan is noisy, pin it statically: in `D:\ghidra_proj2\ER`, find the caller/ancestor of the
factory-table dispatch that leads to `FUN_1407fd4b0` (er+0x7fd4b0) and locate the early-out that checks combat
before opening the map (a `if (in_combat) return;`). That predicate read names the flag (offset on
WorldChrMan/PlayerIns). Cross-ref the warp-in-combat gate for the same read.

## Wire-up (once the flag is known)
- `goblin::combat_active()` (host; resolve the chain once, SEH/RPM-read the byte each call — mirror
  `world_map_open()`'s CSMenuMan read).
- In `panel_virtual_map` (or the host frame tick): `if (goblin::overlay_api::vmap_redirect() &&
  goblin::combat_active()) goblin::overlay_api::set_vmap_redirect(false);` → the vmap closes the instant combat
  starts, exactly like the native map. (The render already mirrors `s_open` to `vmap_redirect`.)
- Same `combat_active()` gates the vmap grace-warp (refuse in combat) — the fast-travel-safety the design docs
  require. Add later with the warp UI.

## Validation (Linux/Proton, game up)
Open the vmap (map key), aggro an enemy → the vmap must auto-close the moment combat starts; after combat ends,
the map key opens it again. Compare to vanilla (native map with `vmap_on_map_key=0`): the vmap-close timing
must match ER's own map-availability.

## Anchors
```
create-callback (hooked)  FUN_1407fd4b0  er+0x7fd4b0   (factory table er+0x2abb910) — combat gate is UPSTREAM
WorldChrMan               WCM_FINDER AOB (re_signatures.hpp) → CS::WorldChrMan → PlayerIns (player-pos chain)
warp gate                 windows_grace_warppin_teleport / LUA_WARP — warp precondition reads the same state
map-open state (ref)      world_map_open() = CSMenuMan[0xCD]==7 (goblin_tutorial_popup.cpp) — for A/B timing
tools                     tools/boot_hold.py (HOLD ER) ; mfg.py rpc mem_dump/mem_fwa ; aggro an enemy live
```
Cross-ref: `native_map_redirect_linux_re_plan.md` (§ redirect + the in-combat follow-up),
`windows_native_map_render_toggle_re_findings.md` §4b (the map↔combat gate = two uses of one state).
