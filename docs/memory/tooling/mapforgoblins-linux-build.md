---
name: mapforgoblins-linux-build
description: How to cross-compile the MapForGoblins MSVC Windows DLL on this Linux machine
metadata: 
  node_type: memory
  type: project
---

ERR-MapForGoblins-DLL (<repo>) is an MSVC Windows x64 DLL (SEH `__try/__except`, MinHook, spdlog, Pattern16, steam_api64). It cross-compiles on this EndeavourOS box via **clang-cl + xwin** (no Wine, no MSVC). Verified working 2026-06-17 → produces valid PE32+ DLL.

Recipe (toolchain file `clang-cl-xwin.cmake` lives in the repo):
```
cargo install xwin --locked
xwin --accept-license splat --output ~/.local/share/xwin   # ~630MB MSVC CRT+SDK
ln -sf xinput.lib ~/.local/share/xwin/sdk/lib/um/x86_64/Xinput.lib  # case fix
cmake -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=clang-cl-xwin.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C build-linux MapForGoblins
```

Gotchas (clang 22 stricter than MSVC): CMakeLists `CMAKE_GENERATOR_PLATFORM x64` must be guarded VS-only; `CMAKE_POLICY_VERSION_MINIMUM=3.5` for minhook's old cmake_minimum; `/arch:AVX2` so clang exposes Pattern16's AVX2/BMI2 intrinsics; `/DFMT_CONSTEVAL=` to neutralize bundled-fmt consteval checks (spdlog uses bundled fmt, SPDLOG_USE_STD_FORMAT got ignored under OLD policy); `#pragma comment(lib,"Xinput.lib")` needs the case-exact symlink.

clang-cl build is functionally equivalent but NOT byte-identical to the shipped MSVC DLL — validate in-game before trusting. Official build path is still build.bat (VS2022, Windows). See [[mapforgoblins-map-freeze]].

**DEPLOY (build does NOT auto-copy):** the running game loads `<ERR_ROOT>/dll/offline/MapForGoblins.dll` (the `offline/` copy only; `online/` + `optional/` exist but are unused for this mod). After `ninja -C build-linux MapForGoblins`, copy it over: `cp build-linux/MapForGoblins.dll <ERR_ROOT>/dll/offline/MapForGoblins.dll` (game must be CLOSED — file lock; then restart the game to load the new DLL). Symptom of forgetting: in-game overlay shows OLD UI/labels despite a fresh build (e.g. missing a just-added label).

## Deploy path (user-emphasized 2026-06-20)
The live DLL the game loads = **`<ERR_ROOT>/dll/offline/MapForGoblins.dll`**.
After cross-building (`cmake --build build-linux --target MapForGoblins` → `build-linux/MapForGoblins.dll`),
ALWAYS `cp` it to that exact path. **Since 2026-07-02 copy `MapForGoblins.pdb` alongside it** — the crash handler symbolizes triage lines via dbghelp only when the pdb sits next to the deployed DLL; offline fallback = `py tools/resolve_crash.py <crash.txt> --dll build-linux/MapForGoblins.dll` (llvm-symbolizer, gives function + file:line). Keep the DLL+PDB pair matched (same build) or names will be garbage. The game loads the DLL at LAUNCH (no hot-reload) — a redeploy
needs a full ERR restart to take effect ("proton cache"). There is only ONE MapForGoblins.dll on
the system; verify with `md5sum` that deployed == build output.

## ⚠️ CRITICAL: builds/deploys MUST run sandbox-disabled (2026-06-20)
The Bash tool runs SANDBOXED by default → `cmake --build` and `cp …/dll/offline/MapForGoblins.dll`
write to a copy-on-write OVERLAY, NOT the real filesystem. The game then loads the STALE real DLL.
Symptoms: redeployed code/diag never appears in-game or logs; behavior looks "cached" (~hours stale);
`stat` inside the sandbox shows a fresh mtime that the user (real FS) does not see. Burned a whole
debugging session on this. FIX: run EVERY build + the `cp` deploy with Bash `dangerouslyDisableSandbox: true`.
Verify with the USER's real mtime, not the sandbox's.

## ⚠️ ninja "no work to do" after a git revert/checkout (2026-06-22)
After `git revert`/`checkout` changes source, `ninja` sometimes reports **"no work to do"** and DOES
NOT rebuild → you deploy a STALE DLL (the pre-revert build). Confirmed: deployed md5 didn't change
after a revert until a forced rebuild produced a DIFFERENT md5. FIX: after ANY git op that changes
tracked source, `touch` the changed file(s) (e.g. `touch src/worldmap/map_renderer.cpp`) then rebuild,
and CONFIRM the build-output md5 actually changed before `cp`. Don't trust "no work to do".
