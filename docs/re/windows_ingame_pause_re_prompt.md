# Windows RE prompt — in-game pause mechanism (PR D, dx_bugs_backlog items 4/5)

Status: OPEN. RE spike, ~half a day, Windows + Ghidra (`D:\ghidra_proj2\ER.rep`, `eldenring.exe`)
required — Linux/Proton can read code but this needs live confirmation against a running game
(Cheat Engine / x64dbg) too. See `docs/plans/dx_bugs_backlog_plan.md` PR D for the full context;
this doc is the standalone RE recipe to execute on the Windows dev box.

## Goal

Find a surgical in-game pause for MapForGoblins (F1 checkboxes: "Pause game" / "Pause while menu
open", `docs/plans/dx_bugs_backlog_plan.md` PR D). Rejected approach: hooking
`QueryPerformanceCounter` process-wide (freezes everything that reads QPC — audio, our own timing,
animation/physics — and a constant tick risks `dt=0` divide-by-zero elsewhere). Wanted instead:
patch the game's own world-update gate, narrowly, the way an established precedent does it.

## Precedent studied (2026-07-01, static read only — no Windows session yet)

[`iArtorias/elden_pause`](https://github.com/iArtorias/elden_pause) (MIT license) implements ER
pause via a **single conditional-jump flip**, not a data-field write or clock hack:

- AOB-scans (own pattern lib) the main module's first section (`.text`, via
  `IMAGE_FIRST_SECTION`) for:
  ```
  0F 84 ? ? ? ? C6 83 ? ? 00 00 00 48 8D ? ? ? ? ? 48 89 ? ? 89
  ```
  — a `je` (`0F 84`, near, 4-byte rel32) immediately followed by
  `mov byte ptr [rbx+disp], 0`, `lea rsi, [rip+disp]`, `mov [rbp-disp], rsi`, `mov [rbp-disp], edi`.
- Toggling = **flip only the opcode's 2 condition bytes** at that address: `0F 84` (je, branch
  taken) ↔ `0F 85` (jne, branch not taken), via `VirtualProtect(PAGE_EXECUTE_READWRITE)` +
  `memcpy(2 bytes)`, restoring original protection after. No new flag, no other bytes touched.
- This forces an EXISTING branch (which already gates something per-object/per-frame, going by the
  `[rbx+disp]` write right after it) to always-skip or always-fall-through, instead of patching
  whatever data the branch normally reads. Net effect in their testing: full world pause, ImGui/UI
  frame-independent overlays (they don't have one, but by construction nothing downstream of THIS
  branch runs while paused — doesn't touch Present/the swap chain).
- Their toggle threads (`GetAsyncKeyState`/`XInputGetState` polling loops) are NOT worth copying —
  we already have a hotkey-poll idiom in `hk_present`/`input_keyboard_poll.cpp`; wire the toggle
  there instead of spawning new threads.
- **Their AOB is for whatever ER build they targeted — do not assume it matches bytes-for-bytes on
  our pinned ERR build/patch.** Same rule as every entry in `src/re_signatures.hpp`: never ship an
  AOB that hasn't been independently confirmed against OUR binary.

## What's NOT yet known (this is the spike)

1. Does `iArtorias`'s exact AOB even resolve one hit in our current `eldenring.exe`? (Game version
   drift alone could break it regardless of ERR.)
2. If it resolves, what function/loop is it actually inside — is `[rbx+disp]` a per-`ChrIns` (per
   character/enemy) update flag, a global world-tick gate, or something else? Does flipping it
   pause animation/physics/AI as a whole, or only one subsystem (e.g. just AI think, leaving
   physics/animation running — would look like ragdoll-only pause, not a clean freeze)?
3. Does flipping it desync anything observable in a short live test (audio glitches, animation
   pop, our own overlay/marker projection reading stale state, savefile/event-flag writes still
   firing while "paused")?
4. Is there a cleaner, more centrally-located gate — e.g. the same flag ER's own menu/cutscene
   freeze uses (`docs/plans/dx_bugs_backlog_plan.md` PR D option 2) — reachable from a class
   already in `rtti_index.txt` (grep for cutscene/pause/menu-state singletons) rather than a raw
   .text AOB with no semantic anchor?

## Verification steps (Windows)

1. **Static: try the AOB as-is.** `query.java`/pyghidra byte-search
   (`docs/memory/tooling/ghidra-re-tooling.md`) for `0F 84 ?? ?? ?? ?? C6 83 ?? ?? 00 00 00 48 8D
   ?? ?? ?? ?? ?? 48 89 ?? ?? 89` against `.text` of the analyzed `eldenring.exe` project. Report
   hit count (want exactly 1) and the enclosing function (decompile via `query.java <addr>`).
2. **If 0 or >1 hits:** the byte pattern has drifted (version or VMP-tier difference) — decompile
   the general area of a KNOWN per-frame world-update call (cross-reference against whatever
   symbol/RTTI anchors are already in `rtti_index.txt` for e.g. `CSChrModule`/`ChrIns` update, or
   the FieldArea per-frame tick) and look for an analogous `je`/branch pattern near a
   `mov byte ptr [rXX+disp], 0` — same SHAPE, different bytes. Derive our own AOB, anchored per the
   `re_signatures.hpp` house style (long-enough prefix, wildcards only where genuinely variant).
3. **Live confirm (Cheat Engine or x64dbg against a running, EAC-disabled ERR):** set a breakpoint
   / watch on the candidate address, trigger the branch during normal play (walk around, let an
   enemy update), confirm which subsystem's advance it actually gates. Then live-patch the 2 bytes
   (CE "Assemble" or a manual byte write) and observe: does world freeze cleanly? Does audio glitch?
   Does our own overlay (already Present-hooked, unaffected by this branch per hypothesis) keep
   rendering/responding normally? Un-patch and confirm normal play resumes.
4. **Decide gate vs QPC fallback** based on 1-3. If a clean single-branch (or single small cluster
   of branches, one per subsystem — animation/physics/AI may each need their own) pause is
   confirmed with no observed audio/desync issue, that's the PR D mechanism; write the finalized
   AOB(s) into `src/re_signatures.hpp` following its existing documentation style (comment above
   each entry: what it resolves, re-find hint, verification note) and update
   `docs/plans/dx_bugs_backlog_plan.md` PR D + this doc's Status line to CONFIRMED/RESOLVED.

## Pointers

- `docs/plans/dx_bugs_backlog_plan.md` — PR D section (options ranked, config/UI already scoped).
- `docs/memory/tooling/ghidra-re-tooling.md` — `query.java`/pyghidra byte-search recipe, project
  paths (`D:\ghidra_proj2\ER.rep` = the analyzed game, NOT `ergg.rep`).
- `src/re_signatures.hpp` — house style for documenting a confirmed AOB + `resolve_all_signatures()`
  startup PASS/FAIL pattern to slot the new signature into.
- `src/goblin_overlay.cpp` `hk_present` — where the existing hotkey-poll idiom lives; wire the pause
  toggle here, not a new spawned thread.
