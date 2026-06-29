# Linux Agent Notes

Linux can do all normal repo work — code, docs, cross-builds, log analysis, and preparing RE prompts
for Windows. Its limits are runtime RE (no live game/debugger here) and Oodle-only assets.

## Build & deploy (Linux)

- The Windows DLL **cross-compiles on Linux** with clang-cl + xwin + ninja (no MSVC/Wine) via
  `clang-cl-xwin.cmake`. Key flags: `/arch:AVX2`, `/DFMT_CONSTEVAL=`, `CMAKE_POLICY_VERSION_MINIMUM=3.5`,
  case-exact `Xinput.lib` symlink. → `tooling/mapforgoblins-linux-build.md`, `tooling/build-toolchain-clang-xwin.md`
- Live DLL path: `<ERR_ROOT>/dll/offline/MapForGoblins.dll` (no hot-reload — restart ERR).
- **Builds/deploys MUST run sandbox-disabled**, or a copy-on-write overlay ships a stale DLL.
- ninja can falsely report "no work to do" after a `git revert` — touch the source and verify the md5.

## Performance gotchas (Proton)

- Per-node `ReadProcessMemory`-to-self is ~free on Windows but a **Proton freeze cliff** (each call ≈
  wineserver IPC). Bulk-read whole structs into a local buffer then parse; throttle re-walks; wrap in
  `GOBLIN_BENCH_QUIET`. → `tooling/linux-rpm-walk-danger.md`, `bugs/collected-refresh-proton-perf.md`
- clang-cl silently elides `__try` around a bare load — use `__try` around a **noinline CALL** for hot
  in-process reads. → `tooling/clang-cl-seh-noinline.md`

## RE limits on Linux

- No live runtime RE (Ghidra-on-running-game, Cheat Engine, RPM against a live `eldenring.exe`).
  Prepare prompts/findings under `docs/re/` for the Windows machine instead.
- Oodle-compressed assets (some MSB/icon extraction) are blocked here; DCX_DFLT/zlib files are fine
  (e.g. talk-ESD via `tools/esd_dump`).
- Linux can still parse disk MSBs, run the no-bake pipeline logic, and do all C++/Python/docs work.
