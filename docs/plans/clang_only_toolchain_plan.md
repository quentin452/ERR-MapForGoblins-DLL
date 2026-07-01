# Clang-only toolchain plan — retire MSVC, one compiler/linker everywhere

Status: **scoped, not started** (build-system audit 2026-07-01). USER DECISION 2026-07-01: go
clang-only. This **reverses** the same-day "MSVC stays release-canonical" policy recorded in
`docs/memory/tooling/build-toolchain-clang-xwin.md` — update that memory doc when Phase 2 lands.

Goal: clang-cl + lld-link + ninja (+ xwin SDK) as the ONLY toolchain, on both Linux and Windows,
for dev AND releases. No VS2022/msbuild/vswhere dependency, no dual-linker drift.

## Why it's already the natural state
- The current Windows box has NO Visual Studio (`build-toolchain-clang-xwin.md`) — `build.bat`'s
  "canonical" path doesn't run on any machine in use.
- `build.bat:151` forces `msbuild /t:Rebuild` purely to dodge MSVC LTCG stale-cache bugs; lld
  thin-LTO doesn't have them (`CMAKE_INTERPROCEDURAL_OPTIMIZATION` works under clang-cl+lld).
- clang builds of all 4 profiles already exist (`build-clang/erte/convergence/vanilla`, 2026-06-24)
  and the Linux cross-build is what's deployed and played on this machine daily.
- Byte-identical-to-MSVC stops mattering once clang IS the reference; lld `/Brepro` gives
  deterministic PE output (better than MSVC).

## Phase 0 — SEH correctness (BLOCKING; real latent crashes in today's clang DLL)

**STATUS 2026-07-01: the 3 sites below are FIXED** (commit `5b80541`, noinline-body pattern;
verified on a no-LTO TU compile: `*_body` symbols emitted, caller keeps the opaque call +
`__C_specific_handler` ref; built + deployed).

**Repo-wide `__try` classify pass DONE 2026-07-02** — every remaining site audited; 2 more
raw-deref-shape sites found and fixed (goblin_crashdump.cpp `image_end` + the mid-crash stack-scan
read — an elided guard there double-faults INSIDE the crash handler). Classification of the rest:

| Site | Shape | Verdict |
|---|---|---|
| goblin_collected.cpp:179/:192 (`safe_read`/`safe_write_byte`) | __try around noinline `raw_copy`/`raw_store8` | SAFE (canonical pattern) |
| goblin_world_position.cpp:529/:594, goblin_tutorial_popup.cpp:89 | noinline `*_body` | SAFE (fixed `5b80541`) |
| goblin_crashdump.cpp:70ish/:156ish | noinline `image_end_body`/`read_qword_body` | SAFE (fixed this pass) |
| goblin_inject.cpp:139/:212 (toast trampolines) | __try around indirect call into game code | SAFE (indirect call ⇒ frame kept) |
| goblin_worldmap_probe.cpp:1498 (`call_reapply_seh`) | __try around `g_reapply_fn` fn-ptr call | SAFE |
| goblin_markers.cpp:669 (`seh_dump_to_file_invoke`) | __try around large cross-TU call | SAFE (never inlined in practice) |
| dllmain.cpp:40/:52/:64/:75/:96 (refresh/init wrappers) | __try around cross-TU calls / fn-ptr param | SAFE in practice (large callees); residual theoretical risk = thin-LTO inlining a small callee — if a dllmain-guarded path ever crashes unguarded, add noinline to the callee |

Phase 0 remaining: in-game fault-injection spot check only (opportunistic — next real crash
exercising a guarded path counts).
Known rule (`docs/memory/tooling/clang-cl-seh-noinline.md`): clang-cl silently ELIDES `__try`
around a raw load/store (even `noinline`); only `__try` around an opaque CALL is preserved.
Repo was converted 2026-06-20 — except these, found in this audit:

1. `probe_player_seh` — `src/goblin_world_position.cpp:511`: `__try` over raw derefs
   (`*(uint8_t**)wcm_static`, `lp+0x6C0/6C4/6C8`). Runs EVERY FRAME (minimap, altitude arrows).
2. `probe_map_pos_seh` — `src/goblin_world_position.cpp:566`: same pattern
   (`singleton+0x2c`, `mgr+0x70/+0x78`). Every frame.
3. `world_map_param_ready` — `src/goblin_tutorial_popup.cpp:69`: `__try` around a call to a
   `static` same-TU function (inlinable ⇒ guard lost) + a direct raw deref `rescap+0x80`.
   Runs during volatile game init — the exact historical crash scenario.

Fix pattern: the SHIPPED `raw_copy` noinline-call pattern from `goblin_collected.cpp`
(`__try{ raw_copy(out,src,n) }` — clang preserves SEH around the opaque call; ~ns, no wineserver
IPC) for the two per-frame probes; plain RPM acceptable for the once-per-init readiness poll.
Then: mechanical pass over ALL `__try` sites in the repo (~31 across 12 files) with a classify
table (CALL-wrapped=OK / raw-deref=convert); spot-verify the hot sites in the built DLL via
`llvm-objdump` (SEH frame present at the probe RVAs).

## Phase 1 — port the build entry point

**STATUS 2026-07-02: IMPLEMENTED + WINDOWS DEFAULT-BUILD VALIDATED.** Done on Linux:
- `/Z7` + `/debug` + **`/Brepro`** in `clang-cl-xwin.cmake` — determinism PROVEN (relink → identical
  md5) on the Linux cross-build.
- `build.bat` fully ported: vswhere/VsDevCmd/msbuild/.sln GONE → ninja + the same toolchain file
  (env-overridable tool paths `MFG_LLVM_BIN`/`MFG_NINJA`/`MFG_CMAKE`/`MFG_XWIN`, defaults = the
  current Windows box). Profile matrix / generate / snapshot / release / bump / package kept;
  output paths moved `%BUILD_DIR%\Release\` → `%BUILD_DIR%\`; ERR build dir `build/` → `build-err/`
  (old msbuild tree disposable). The Rebuild-LTCG hack is gone; a cached `VERSION_PRE=""` from a
  release run is now explicitly reset to `pre` on the next configure (latent bug in the old script).
- PDB archiving: snapshot/release copy the DLL+PDB pair to `pdb-archive/<ver>-<profile>/`
  (git-ignored, NOT shipped) for `tools/resolve_crash.py`.
- Guardrail: `tools/lint_seh.py` — flags any `__try` body containing a raw deref (comment/string
  aware); currently clean, verified it catches the bad shape.
**Windows validation (2026-07-02):** `build.bat` (default = ERR profile) ran clean end to end on the
Windows box — auto-configure (CMake 4.1 + Ninja + `clang-cl-xwin.cmake`, Clang 22.1.8), `[80/80]`
compile+link → `[SUCCESS] MapForGoblins`, `build-err/{MapForGoblins.dll 4.6 MB,.lib,.pdb}` produced.
0 real errors (the sole `Failed` = `CMAKE_HAVE_LIBC_PTHREAD` probe, expected/no-op on Windows). 340
warnings, all benign third-party/deprecation: `-Wdeprecated-literal-operator` (spdlog bundled fmt
`operator"" _a`) + `-Wdeprecated-declarations` (`std::wstring_convert`/`<codecvt>` in
`src/from/params.hpp:17`, still functional) — suppressible via
`_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING` if a clean log is ever wanted.
**Phase 1 close-out DONE 2026-07-02 — `build.bat snapshot` VALIDATED on Windows.** Full snapshot
(`pre-1.0.18`, ERR) ran clean end to end: data pipeline (964 MSB, 28313 placements, 0 MSB errors) →
reconfigure `-DVERSION_PRE=pre` → `[64/64]` link `[SUCCESS]` → `mfg_inigen` wrote the INI → package
laid out under `pre-release/` (`dll/offline/{dll,ini}` + `addons/MapForGoblins/menu/02_120_worldmap.gfx`
+ LICENSE) → **PDB pair archived to `pdb-archive/pre-1.0.18-err/` (DLL + 27 MB PDB)** → README version
substituted (`vpre-1.0.18`). Crash-symbolication chain verified: the shipped DLL is **byte-identical**
(SHA-256) across `pre-release/`, `pdb-archive/`, and `build-err/`, and the PDB is archived but NOT
shipped in the package. All that remains is Phase 2 (in-game validation matrix + docs flip + delete
`steam_api64.lib`).
- Replace `build.bat`'s configure/build (vswhere+VsDevCmd+`.sln`+msbuild) with Ninja + the existing
  `clang-cl-xwin.cmake` on Windows (paths per `build-toolchain-clang-xwin.md`: scoop LLVM,
  `D:\mfg_toolchain\xwin-sdk`, `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, Release-only).
- KEEP unchanged: profile matrix (err/vanilla/convergence/erte → `GENERATED_SUBDIR`, package
  layouts), `generate` (build_pipeline.py), `snapshot`/`release` packaging via `mfg_inigen.exe`,
  version parse/bump. Only the compile/link step changes.
- Add `/Brepro` (deterministic PE) + emit PDB (`lld-link /debug`) and ARCHIVE the PDB per
  release/snapshot — replaces MSVC-parity as the crash-RVA decode story. Old shipped MSVC releases
  stay decodable only via their archived binaries; keep them.
- Unify build dirs: `build-<profile>` all-clang; retire `build/` (msbuild) and fold
  `build-linux`/`build-clang` naming.

## Phase 2 — validation + flip

**STATUS 2026-07-02: docs flipped + steam_api64.lib removed.** In-game validation is de-facto
satisfied — the clang DLL is the daily-played build and today's session exercised the historical
SEH sites (collected refresh, probe page transitions, icon harvest, minimap/altitude). README
build instructions rewritten (clang/ninja/xwin, both hosts); memory policy note flipped to
"clang = THE toolchain". `build.bat snapshot` now VALIDATED on Windows (2026-07-02, `pre-1.0.18`):
packaging + `pdb-archive` proven, shipped DLL byte-identical across package/archive/build-err (see
Phase 1 close-out above). Remaining: `build.bat release` un-exercised (first real release run will
prove the version-bump path), the ACTUAL in-game matrix, optional CI guardrail.
- In-game pass on ERR + a spot-check profile, exercising the historical SEH crash sites:
  kindling heap scan, collected refresh (tile churn), worldmap probe across a DLC/underground page
  transition, icon harvest with inventory/map churn, altitude arrows + minimap on (Phase 0 sites).
- Then: update `docs/memory/tooling/build-toolchain-clang-xwin.md` (clang = canonical) +
  `docs/memory/linux.md`/`windows.md` pointers, README build instructions, delete the msbuild path
  from `build.bat` (or replace the file), remove `steam_api64.lib` from repo root (linked nowhere;
  runtime uses `GetModuleHandleA("steam_api64.dll")` only — verify then delete).

## Guardrails (with Phase 1)
- Lint script (pre-build or CI): flag any `__try` block containing a direct deref/`memcpy` on a
  non-local pointer without going through a registered safe wrapper — the elision regression is
  silent, so grep is the only cheap tripwire.
- Optional: GitHub Actions Linux runner doing the xwin cross-build per PR (repo has NO CI today).

## Known limitations to document as official
- **Release-only**: xwin has no debug CRT (`libcmtd`) — Debug config cannot link. Already the
  de-facto state.
- **`/arch:AVX2` is global** under the toolchain file (Pattern16 needs the intrinsics exposed) ⇒
  clang may auto-vectorize AVX2 anywhere ⇒ hard CPU floor = AVX2. ER min-spec CPUs all have it;
  if ever a report surfaces, scope the flag to the Pattern16-using TUs instead.
- Packaging (`mfg_inigen.exe`) runs the tool natively ⇒ release packaging happens on Windows
  (or wine on Linux — not needed today).
