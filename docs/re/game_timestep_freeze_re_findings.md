# RE FINDINGS (2026-07-06, Windows live) — game timestep / clean-freeze lever

Answers the 3 RE questions in `game_timestep_freeze_re_prompt.md`. Done **live** on the running game
(Windows box, ER 2.6.2.0, `er_base=0x7ff762b90000` this session) via external `ReadProcessMemory` +
capstone disasm of `FUN_140623410` (er+0x623410) and its callers — faster than a Ghidra headless run since
the anchor RVA was already known. RVAs below are build-pinned (App 2.6.2.0); addresses with `er+` are
module-relative and stable across boots, live heap/globals are session-specific.

## Q1 — where does the `dt` come from? → a pure `xmm1` register ARG (no time-scale global)

`FUN_140623410`'s head stores its incoming 2nd float arg straight to a local and passes it on unscaled:

```
er+0x623456: movss [rbp-0x71], xmm1      ; dt (arg) -> local
er+0x62349d: movss xmm1, [rbp-0x71]      ; reload, pass to child er+0x643bc0
er+0x6234a7: movss xmm0, [rbp-0x71]      ; reload
er+0x6234cc: movss [rbp-0x19], xmm0      ; store into the stack FD4Time.deltaTime  (see Q3)
```

No `mulss`/scale is applied inside the function — `dt` flows through raw. The head's repeated
`mov rcx,[rip+X]; test; jne; lea rcx,[rip+Y]; call er+0x1ec13e0 … call er+0x1eb97a0` blocks are MSVC
thread-safe local-static init guards, **not** the dt (their `[rip]` globals are guard vars/pointers).

**The 7 call sites all pass raw `[timeObj+8]`** (region `0xaf6xxx`, uniform shape):

```
movss xmm1, [rsi+8]        ; dt = timeObj.deltaTime  (offset +0x08)
mov   rcx, [rbx+0xf0]      ; the subsystem-group manager
call  er+0x623410
```

Callers (7): `er+0xaf6381, 0xaf67ca, 0xaf68fb, 0xaf6bdd, 0xaf96ec, 0xb0117a, 0xb0118c`. No `mulss` by a
global anywhere on the path → **there is NO writable time-scale multiplier global** (prompt path A as a
"scale global" is not available).

## Q3 — `FD4Time` layout → `deltaTime` at **+0x08**

`FUN_140623410` builds an `FD4Time` on the stack at `[rbp-0x21]` each call:

```
er+0x6234ac: lea rbx,[rip+..] -> er+0x29c8e48   ; vftable FD4TimeTemplate<float>  (RTTI .?AV?$FD4TimeTemplate@M@FD4@@)
er+0x6234b3: mov [rbp-0x21], rbx                ; +0x00 = vftable
er+0x6234c1: lea rdi,[rip+..] -> er+0x29c8e58   ; vftable FD4Time                 (RTTI .?AVFD4Time@FD4@@)
er+0x6234c8: mov [rbp-0x21], rdi                ; +0x00 = 2nd-base vftable
er+0x6234cc: movss [rbp-0x19], xmm0             ; +0x08 = deltaTime = dt
er+0x62350b: lea rdx,[rbp-0x21]                 ; &FD4Time -> subsystems
```

So `FD4Time = { +0x00 vftable(s), +0x08 float deltaTime }`, and it's the **same +0x08** the callers read
from `timeObj`. The two vftable RVAs (`er+0x29c8e48`, `er+0x29c8e58`) match the RTTI index
(`tools/ghidra/rtti_index.txt`).

## Q2 — global time-scale multiplier? → NOT on this path

Not found: dt is a raw arg and the callers read raw `timeObj+0x08`. The single upstream value is the
**master deltaTime = `timeObj+0x08`** (an FD4Time-layout field), written once per frame by the frame timer
and read by all 7 dispatch groups. There is no `dt * scale` global to poke.

## Implementation paths (revised from the prompt)

- **B — hook `FUN_140623410`, scale `xmm1` — CONFIRMED, recommended default.** Single hook catches all 7
  dispatch groups; `dt` is a clean `xmm1` arg. `set_timescale <f>`: `xmm1 *= g_scale` in the trampoline
  (`0` = freeze → nothing accumulates → instant resume, fixing the branch-flip resume-latency bug). AOB must
  be **body-anchored or RVA-pinned** — the `sub rsp,0xB8` prologue is not unique.
- **A' — patch the frame-timer that WRITES `timeObj+0x08` (best, hook-free at the source).** Zeroing/scaling
  the master deltaTime at its writer freezes every downstream consumer from one point (safer than B if any
  subsystem reads dt off a path that doesn't funnel through er+0x623410 — unverified). **Find the writer via
  an FWA-WRITE on `timeObj+0x08`** → needs the **`mem_fwa off` disarm verb** first
  (queued in `windows_enemy_name_hud_feed_re_findings.md`; the single FWA slot wedges). This is the next
  live step.
- **C — native map-open freeze** (orthogonal cross-check) unchanged from the prompt.

## Open / next (Linux daily-build box)

1. Land `mem_fwa off` (unblocks this AND the enemy-name capture).
2. Find `timeObj` live + read `timeObj+0x08` (expect ~0.0166 @60fps; 0 during loading). Trace `rsi` up from
   the `0xaf6xxx` dispatcher, or FWA-write `timeObj+0x08` → the writer = the frame timer (path A' lever).
3. Decide A' (patch the writer / write-at-source) vs B (hook er+0x623410). Prototype: `mem_dump` the master
   deltaTime, write 0 → world should freeze with instant resume; write back to confirm.
4. Verify: pause 60 s → resume hitch ≈ 0 (vs the branch-flip's duration-proportional hitch); no audio/anim
   pop at dt=0. Then wire `set_timescale <f>` RPC + rework `goblin::pause::set_paused` off the branch flip;
   AOB into `src/re_signatures.hpp`; update `windows_ingame_pause_re_prompt.md` + `dx_bugs_backlog_plan.md` PR D.

## Method note (reusable)

Live disasm beat static here: RPM-read the function bytes at `er_base+RVA`, `capstone` (CS_MODE_64) with
`detail=True`, resolve `[rip+disp]` globals to `er+RVA` and `e8/e9 rel32` to call targets; scan the whole
module image for `e8`/`e9` whose target == fn to enumerate callers, then disasm a window before each. Scripts
(`ts_disasm.py`, `ts_caller.py`) were in the session scratchpad (ephemeral); the recipe is the durable part.
