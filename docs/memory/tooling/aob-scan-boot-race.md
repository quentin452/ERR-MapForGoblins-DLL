# Gotcha — never AOB-scan at DLL-init / thread-start; resolve lazily

**Status:** active gotcha (learned 2026-07-04, `3e6539f`).

Calling `modutils::scan` (AOB scan of `eldenring.exe`) too early — at DLL init, in `setup_mod`, or
at the start of a background thread that's spawned during boot — can **crash instantly** with
`0xC0000005` inside `Pattern16::Impl::scanRegion`. The DLL's `install()` runs during
"Waiting for params..."; a thread it spawns then scans the game module before its sections are fully
mapped / while modutils' `candidate_modules` is still being set up → fault.

**Symptom:** instant crash on launch; `logs/MapForGoblins_crash_<pid>.txt` shows
`fault_symbol = Pattern16::Impl::scanRegion+…` with the stack `modutils::scan ← <your fn> ← thread`.

**Rule:** resolve AOB-anchored statics **lazily, on first real use**, not eagerly at boot. Every
working resolver in the repo already does this:
- `goblin_world_position.cpp` — `resolve_world_chr_man()` runs on the first `get_player_*` call.
- `goblin_warp.cpp` — `initialize()` runs on the first `to_grace`.
- `goblin_load_watchdog.cpp` — resolves on the first warp-arm (poll loop idles until then), NOT at
  thread start. The original eager `resolve_statics()` at `watchdog_loop` entry was the crash.

By first-use (player moves, warps, opens a menu) the game module is fully mapped and the scan is safe.
The **freeze watchdog** is safe to spawn at boot precisely because it never scans — it only reads an
atomic beat. If a boot-time thread MUST have a slot, gate the scan behind a "params loaded" signal or
a delay, but lazy-on-use is the simplest correct pattern.

Related: [mapforgoblins-linux-build](mapforgoblins-linux-build.md) (build/deploy), the SEH-noinline
pattern for the derefs themselves ([clang-cl-seh-noinline] in `docs/memory/tooling/`).
