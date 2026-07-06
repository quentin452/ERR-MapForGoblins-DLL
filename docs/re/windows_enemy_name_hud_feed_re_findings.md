# RE FINDINGS (Windows, 2026-07-06) — enemy-name HUD feed (`01_000_fe.gfx` / `EnemyTag_ColorText`), live recon

Progress toward `docs/plans/native_enemy_names_scaleform_plan.md`. This is a **live external-RPM recon**
session on the running game (Windows box, user-launched ER); it did NOT yet answer the plan's mod-agnostic
GATE (Q1) or capture the engine name-feed write site. It hands off to the Linux (daily-build) agent with a
concrete next step + one small DLL enhancement needed. Read the plan first.

- **Platform/build:** `er_version = 2.6.2.0`. Live DLL `built = Jul 5 2026 09:41:48` (STALE vs Linux daily
  build — missing the Jul-6 accent fix `d23b779`; see the two standing Windows rules in
  `docs/memory/windows.md`). Game in-world at Gatefront (`coords area=60 grid=(42,36)`), **language =
  French** (on-screen boss name = "Sentinelle de l'Arbre" = Tree Sentinel).
- **Method:** external `ReadProcessMemory` scanner (Python 3.14 ctypes + kernel32), attach to live
  `eldenring.exe`, enumerate committed readable regions, substring-scan (ASCII + UTF-16LE), pointer-follow.
  Same RTTI/heap approach that beat static Ghidra on the sibling worldmap movie
  (`worldmap_native_clip_b3_scaleform_re_findings.md`). Scripts were in the session scratchpad (ephemeral);
  the needle list + method are embedded below so they're reproducible on any box.

## ✅ CONFIRMED LIVE (was offline-`strings` only before)

1. **The field-HUD movie `01_000_fe.gfx` is loaded**, resolved from the ACTIVE install (mod-agnostic by
   construction): live GFx runtime objects `MovieView "01_000_fe.gfx"` + `MovieData "01_000_fe.gfx"`, and
   the disk path `../../mod\menu\01_000_fe.gfx` (ERR's replaced movie — the engine loaded the install's own
   file, not a baked constant). Same pattern the worldmap movie showed.
2. **`EnemyTag_ColorText_12` is the name TextField** — live symbols `_01_000_FE_fla::EnemyTag_ColorText_12/Text_0`
   (the `/Text_0` child = the actual `flash.text.TextField`), plus a live display-object node
   `EnemyTag_ColorText_12` surrounded by GFx pointers. Siblings in the same tag: `EnemyTag_HP_Ani_9`,
   `EnemyTag_Loss_Ani_7`.
3. **The 8 tags = `EnemyTag0..EnemyTag7` + `M0EnemyTag0`**, a contiguous instance array (matches the
   `entityHpBars[8]` we already walk), plus a second live instance region carrying vtable/parent pointers
   (`EnemyTag1..7` + `M0EnemyTag0` with `&..@`-style heap ptrs). Name-plate + enemy-HP assets resident:
   `MENU_FE_NameBase`, `MENU_HP_Base_Enemy`.
4. **★ The name is fed to the tag as Scaleform HTML via SetTextHTML.** The live buffers for the currently
   displayed name (a NAMED field boss) were both found:
   - **HTML source** (what the engine hands to the TextField): `<FONT LETTERSPACING='0'>Sentinelle de
     l'Arbre</FONT></TEXTFORMAT>` (ASCII/UTF-8, near `0x27a10bf9915`).
   - **Rendered content** (the TextField's internal doc, UTF-16LE): `Sentinelle de l'Arbre\0` followed by
     GFx string metadata (`… 7a 02 00 00 <ptr 0x27a106e66f0> 00 00 00 00 ff ff ff ff`) at `0x27a106e6a70`.
   - Both sit in the **same heap arena as the `01_000_fe.gfx` MovieView/MovieData + EnemyTag nodes**
     (`0x27a10…`) → this is the live HUD text, not an FMG copy.
   - **Implication for the RE:** the name-feed hook is whoever BUILDS that `<FONT …>name</FONT>` HTML and
     calls `SetTextHTML`/`SetVariable` on the EnemyTag path. Watching a WRITE to the HTML-source buffer
     catches the engine name-feed; watching the UTF-16 buffer catches GFx's post-parse text set.

> ⚠ **All hex addresses above are SESSION-SPECIFIC** (heap + ASLR — `er_base` was `0x7ff762b90000` this
> session). Re-scan every boot. They are recorded only to show the shape, not to be hardcoded.

## ❓ STILL OPEN — the mod-agnostic GATE (Q1) is NOT answered

Q1 (does VANILLA display `EnemyTag_ColorText` when fed a name, or hide it for non-boss?) is **behavioral** —
it needs a `SetText` + observe, or the write-site captured then reasoned about. Not done. Do NOT delete the
ImGui path until Q1 = "displays when fed". (This session only proved the ERR case: the tag shows a named
boss.)

## 🔬 Pause finding (answers "can find-what-writes trigger while paused?") — NO for writes

- A hardware **write** breakpoint fires only when the writing INSTRUCTION executes. ER's "pause the game"
  freezes the game/HUD update tick where the name-feed writes the tag → **the write never runs → a write
  BP stays armed but never fires while paused.** Confirmed the value is already resident while paused (read
  "Sentinelle de l'Arbre" fine), i.e. it was written before the pause.
- **Nuance:** a find-what-**READ** may still fire while paused, because the present/render thread keeps
  drawing the frozen frame (it reads the text to render it). So: **writes need UNPAUSE; reads may not.**
- **Consequence for capture:** pause is the right state to LOCATE + ARM; you must **UNPAUSE and refresh the
  name** (look away/back at the enemy, or acquire a fresh named target) so a write executes and the BP hits.

## 🚧 BLOCKER hit this session + the small DLL enhancement needed (for the Linux agent)

`mem_fwa` (`src/goblin_debug_rpc.cpp` → `goblin::field_probe::arm_raw`) **already supports write-only**
(`make_dr7`: `rw = write_only ? 0b01 : 0b11`, `src/goblin_field_probe.cpp:55`), so `mem_fwa <addr> <len> w`
IS a real find-what-writes. Two real gaps blocked us:

1. **★ No disarm verb → the single FWA slot wedges.** `field_probe` holds ONE global arm (`g_armed`), and
   auto-disarm only fires after `FWA_MAX_HITS` *distinct* RIP sites. This session a **stale `@geom` probe**
   (armed 01:20 on `CSWorldGeomIns.alive = 0x27a2d74a1e3`, logged 3 hits then stuck below the threshold)
   held the slot, and `mem_fwa` refused every new arm (`[FWA] already armed … — ignoring arm request`).
   There is `field_probe::disarm()` (clears DR0 on all recorded threads) but **no RPC reaches it**.
   → **✅ LANDED 2026-07-06:** `mem_fwa off` (alias `disarm`) verb → `goblin::field_probe::disarm_reset()`
   clears DR0 on all threads, removes the VEH, deletes the critical section, and resets
   `g_armed`/`g_disarmed`/`g_seen`/`g_watch_addr` so a fresh arm proceeds. `arm()` is now re-armable
   (CS init-once guard). Host-only (`goblin_field_probe.{hpp,cpp}` + `goblin_debug_rpc.cpp`).
2. **Log mislabeled writes as "READ".** → **✅ FIXED 2026-07-06:** the hit line now prints WRITE/READ from
   the watch's `write_only` (`g_write_only` recorded at arm time).

(Neither is the plan's blocker — they're just what stopped the live capture on Windows today.)

## ➡ NEXT STEP (recommended, for the Linux daily-build box)

Linux is the better box here (daily build + in-DLL FWA proven, `linux-runtime-re-options.md`). Do:

1. Land the tiny DLL enhancement above (disarm verb + write/read log label). Rebuild → restart ER.
2. In-world near a NAMED enemy (a hostile NPC/invader gives an EnemyTag name directly; a field boss like
   Gatefront's Tree Sentinel also works). **Locate the HTML-source buffer live** (re-run the scan below).
3. `mem_fwa <html_buf_addr> 4 w` to watch the engine writing the `<FONT …>name</FONT>` string. **Keep the
   game UNPAUSED** and **refresh the name** (look away then back, or acquire the target) so the write fires.
4. Read `[FWA]` in `…/dll/offline/logs/MapForGoblins.log`: the hit RIP (`er+0x…`, already RVA) + the caller
   ret-addr chain = the **engine name-feed function**. Author the AOB with `offset_resolver.py`.
5. With the hook found: for tags the engine leaves blank (nameId==0 generics/sheep), substitute
   `goblin::enemy_display_name(npcParam, model)` (tiers 2/3 from `windows_enemy_name_runtime_source_re_findings.md`).
6. Answer Q1 by testing on VANILLA (feed a name to a non-boss tag → does it render?). Only then decide
   delete-ImGui vs keep-as-fallback.

## Reproduce — the external RPM scan (Windows; portable method)

Attach to `eldenring.exe`, enumerate committed readable regions (`VirtualQueryEx`, protect ∈ {R,RW,ER,ERW},
skip PAGE_GUARD), `ReadProcessMemory` each in ≤64 MB chunks, substring-search. Needles that located
everything above:

- Movie / symbols (ASCII): `01_000_fe`, `EnemyTag_ColorText`, `_01_000_FE_fla`, `M0EnemyTag0`,
  `EnemyTag1`, `EnemyTag7`, `MENU_FE_NameBase`, `MENU_HP_Base_Enemy`.
- Live on-screen name (ASCII **and** UTF-16LE — the GFx buffers are wide): the displayed name substring,
  e.g. `Sentinel`/`Tree Sentinel`. The HTML-source hit is the one wrapped in `<FONT LETTERSPACING='0'>…
  </FONT></TEXTFORMAT>`; the UTF-16 hit in the `0x27a10…` arena is the TextField content.

On Linux use the equivalent (`process_vm_readv` / `/proc/<pid>/mem`, module base from `/proc/<pid>/maps`)
or an in-DLL probe. `mem_dump <addr> <len>` (RPC) confirms a candidate through the DLL's own view before
arming.

## ⇒ LINUX LIVE CAPTURE PROGRESS (2026-07-06, headless) — buffer located live; write-RIP one step away

Ran the whole flow headless on the daily-build box (fresh `mem_fwa off` disarm verb + the new
present-heartbeat build). Real progress; the write site itself not yet captured.

- **Full headless loop proven:** cold-boot ER via me3 (`boot_hold.py`, no RPC-ping in the hold loop —
  a persistent ping CONTENDS with one-shot `mfg rpc` and hangs), load save, drive to the **Tree Sentinel**
  at Gatefront (move with **`key w`** — forward is `w`/WASD, NOT AZERTY `z`), engage → the **boss bar
  "Tree Sentinel"** shows. `pause` to freeze the fight (survive) while scanning; `frame=` in `status`
  confirms the render thread stays alive while paused.
- **Located the live HUD name buffer:** the SetTextHTML source `<FONT LETTERSPACING='0'>Tree Sentinel
  </FONT></TEXTFORMAT>` (ASCII/UTF-8), e.g. `0xde27bdec` this session. When NOT engaged, the same slots
  sit EMPTY as `<FONT LETTERSPACING='0'></FONT></TEXTFORMAT>` (found in the pre-engage scan) → these are
  **pre-allocated TextField HTML slots**, written into (likely in-place) when a name shows.
- **`mem_fwa <buf> 4 w` arms fine** (105 threads, write-watch) — the disarm verb + WRITE/READ label work.
- **The write is ONE-SHOT on bar-show:** unpausing with the bar already up did NOT re-fire (no per-frame
  rewrite); damaging the boss did NOT re-fire (HP fill ≠ name text). So a watch armed AFTER the bar shows
  never catches its (already-past) write.
- **The buffer is freed on DEATH/RELOAD** (a full reload re-lays the GFx arena → `0xde27bdec` became
  pointers, 0 hits). So death is fatal to the address; a **no-reload de-aggro** is required.
- **★ SAFETY LESSON (caused the D-state freeze earlier):** scan **rw-p PRIVATE/anonymous heap only**,
  size- + total-capped (`scan2.py`). The first pass (`scan.py`) read the WHOLE 4 GB address space incl.
  swapped/reserved regions → massive swap-in → the game wedged in **uninterruptible D-state** (unkillable
  until the I/O returned; needed the GPU/IO to abort). NEVER full-address-space scan a live game.

### ⇒ FINISH RECIPE (next run — the write-RIP is ~1 iteration away)
1. Engage the boss → `pause` → scan (`scan2.py`, heap-only) → note buffer `Y`.
2. **De-aggro WITHOUT a reload** so `Y` stays put: `warp <gatefront_grace>` (test if the soft-load keeps `Y`
   stable via `mem_dump Y`), OR flee to break the leash (no death). Confirm `Y` is back to the empty
   `<FONT…></FONT>` in-place.
3. `mem_fwa Y 4 w` while the bar is down.
4. Re-engage → the engine writes the name back into `Y` → `[FWA] … WRITE … rip=er+0x…` = the name-feed
   function. Author the AOB with `offset_resolver.py`.
5. **Easier alternative:** use a NON-boss NAMED enemy (a hostile NPC/invader) — its small **EnemyTag**
   nameplate clears when it leaves the screen and re-populates on look-back, so the refresh is a trivial
   camera turn (`mouse_drag x0 y0 x1 y1`, needs 4 args) with NO combat/reload — arm the EnemyTag buffer,
   look away, look back → write fires.
