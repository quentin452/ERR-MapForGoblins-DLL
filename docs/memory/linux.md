# Linux Agent Notes

Linux can do all normal repo work — code, docs, cross-builds, log analysis, and preparing RE prompts
for Windows. Its limits are runtime RE (no live game/debugger here) and Oodle-only assets.

## Build & deploy (Linux)

- The Windows DLL **cross-compiles on Linux** with clang-cl + xwin + ninja (no MSVC/Wine) via
  `clang-cl-xwin.cmake`. Key flags: `/arch:AVX2`, `/DFMT_CONSTEVAL=`, `CMAKE_POLICY_VERSION_MINIMUM=3.5`,
  case-exact `Xinput.lib` symlink. → `tooling/mapforgoblins-linux-build.md`, `tooling/build-toolchain-clang-xwin.md`
- Live DLL path: `<ERR_ROOT>/dll/offline/MapForGoblins.dll` (no hot-reload — restart ERR).
- **Deploy ATOMICALLY while the game runs** (user-flagged 2026-07-03): a plain `cp` over the live DLL
  can leave a partial/half-written file that crashes on the next launch. Copy to `MapForGoblins.dll.tmp`
  then `mv -f` into place (mv is atomic on the same FS → launcher sees old-complete or new-complete,
  never partial) and verify `md5sum src == dst`. The new DLL only takes effect on ERR restart anyway
  (no hot-reload in the single-DLL build).
- **Builds/deploys MUST run sandbox-disabled**, or a copy-on-write overlay ships a stale DLL.
- ninja can falsely report "no work to do" after a `git revert` — touch the source and verify the md5.

## Performance gotchas (Proton)

- Per-node `ReadProcessMemory`-to-self is ~free on Windows but a **Proton freeze cliff** (each call ≈
  wineserver IPC). Bulk-read whole structs into a local buffer then parse; throttle re-walks; wrap in
  `GOBLIN_BENCH_QUIET`. → `tooling/linux-rpm-walk-danger.md`, `bugs/collected-refresh-proton-perf.md`
- clang-cl silently elides `__try` around a bare load — use `__try` around a **noinline CALL** for hot
  in-process reads. → `tooling/clang-cl-seh-noinline.md`

## RE on Linux (updated 2026-07-02 — the old "no live RE here" claim was WRONG)

- The live game runs on THIS box (Proton, `~/Games/ERRv2.2.9.6`) and runtime RE works via
  **in-DLL probes**: `src/goblin_param_scan.cpp` ([PARAMSCAN]/[EMEVDSCAN]/[ABPTEXT]/[ASSETRADAR]/
  [ASSETCOUNT], `debug_logging`-gated; edit needles/dump lists + rebuild ~1 min, one game restart
  per round) + offline python over the dumped .txt files. Proven end-to-end on Group 2
  (`docs/re/linux_group2_prompt_binding_re_findings.md`). Default to this before reaching for the
  Windows PC; see [[linux-runtime-re-options]] for the option ranking (ceserver+CE GUI untried).
- Still Windows: Cheat Engine GUI comfort and the existing Ghidra project/scripts (Ghidra itself
  runs natively on Linux — that's habit, not a constraint).
- Oodle-compressed assets (some MSB/icon extraction) are blocked here; DCX_DFLT/zlib files are fine
  (e.g. talk-ESD via `tools/esd_dump`).
- Linux can still parse disk MSBs, run the no-bake pipeline logic, and do all C++/Python/docs work.
- **Params are live at the TITLE SCREEN — no save needed.** The resident param tables are populated
  when the engine parses `regulation.bin` during boot (~t+7-9s, well before any save load — the DLL's
  `Waiting for params...` gate clears at the title). So any param-only RE or verification
  (`from::params::get_param`, `goblin::paramedit::param_get/set_field`, the RPC `param_get`/`param_set`
  commands) works against a game sitting on the title screen — you do NOT need to load a save or drive
  the world. Save/pixels are only needed for world-state features (positions, event flags, map).
- **A background Claude job CAN launch ER for a self-contained RPC run** (updated 2026-07-03) — via
  the foreground in-shell-child pattern (`GameSession` / `tools/rpc_tests/test_*.py`), NOT detached
  (setsid/disown/run_in_background get reaped at param-wait, exit 144). **Hard prereq: Steam must
  already be running** or me3 aborts `require_steam` — start it once headless (`steam -silent &`;
  auto-login persists + daemonizes + survives across tool calls). goods_count offsets were
  live-verified this way (2026-07-03). For a LONG-LIVED interactive game the user still launches it
  (a foreground tool call kills its child on return). Details in [[tooling/mfg-rpc-driver-hardening]].
