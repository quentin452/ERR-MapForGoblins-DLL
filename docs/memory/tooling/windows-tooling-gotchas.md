---
name: windows-tooling-gotchas
description: "Hard-won Windows/sandbox tooling quirks on this machine (cmd/bat, FS overlays, redirects, disk)"
metadata: 
  node_type: memory
  type: feedback
---

Environment quirks discovered on this Windows box — they cost hours; apply directly:

- **Running `.bat` files:** `cmd.exe /c "x.bat ..."` via the **Bash tool** only prints the cmd
  banner and does NOT run the command (nested-quote/bridge issue). **Use the PowerShell tool** to
  invoke `.bat` (e.g. `& "C:\path\tool.bat" args`). Same for `build.bat` — or bypass it and run the
  Python directly.
- **Sandbox FS overlays:** each tool call gets its own copy-on-write FS snapshot. A directory created
  with `New-Item`/`mkdir` in one call is **NOT visible** to a background process spawned in another
  call (e.g. Ghidra: `java.io.FileNotFoundException: Directory not found`). Fixes that work:
  create the dir **in the same call** that uses it, OR create it via the **Write tool** (writes the
  real FS). `dangerouslyDisableSandbox: true` did **not** fix it. Pre-existing dirs (from before the
  session) are visible fine.
- **Output capture:** a custom `*>`/`>` redirect to a `D:\...log` can silently go stale/unwritten
  across background calls (I diagnosed off a 1-hour-old frozen log). Prefer reading the **background
  task's own output file** (the path returned by the tool), `tr -d '\000'` for PowerShell UTF-16.
- **Disk:** `C:\` is tight (~13 GB free); `D:\` has ~600 GB. Put big artifacts (Ghidra projects,
  extracted mods, downloads) on **D:\**. The Write tool can't `mkdir` at a drive root (`D:\` EPERM)
  but can create files in subdirs.
- **Bash tool eats backslashes in env-var paths:** `PYTHONPATH=C:\Users\...` passed inline to the
  **Bash tool** gets each `\X` unescaped away → `C:Users<user>AppData...` (relative, junk), so the
  mmap shim silently never loads and `extract_items` dies with `PermissionError WinError 5` on
  `os.unlink` of a still-mmapped temp `.bnd`. **Fix: forward slashes** —
  `PYTHONPATH="<windows_temp>/mfg_aux"` (Python accepts `/` on Windows).
  Verify with `py -3.14 -c "import os; print(os.getpid())"` → a huge number means the shim is active.
- Git: repo had **dubious-ownership** (owned by Administrators); fixed once with
  `git config --global --add safe.directory <repo>`.
- **SoulsFormats temp-file mmap, per-CALL not per-process (2026-07-01, `tools/_find_npc.py`):** the
  `frombytes()` helper wrote SoulsFormats input to a `<pid>_fn<ext>` temp path — fixed per process. But
  `BND4.Read`/`MSBE.Read` keep the temp file **memory-mapped for the process lifetime**, so the 2nd
  BND read in the same run collided on that one path (`IOException: file has an open user-mapped
  section`), and even the 1st run's `os.unlink` threw `WinError 5`. Fix: **unique temp name per call**
  (`<pid>_fn<seq><ext>`) + best-effort `try/except OSError` unlink (the OS reaps the temp dir; the
  mapping just leaks until exit). Same WinError-5 family as the `extract_items` PYTHONPATH bullet
  above, different root cause.
- **[OBSOLETE 2026-07-02 — single-DLL migration deleted gen_nonerr_stubs.py and the per-profile
  bake dirs entirely; kept for the lesson only.]
  `tools/gen_nonerr_stubs.py` was only-if-MISSING → silent stale-schema build break (2026-07-01):**
  it emits the non-ERR (`generated_erte`/`_vanilla`/`_convergence`) stubs of the ERR-only generated
  files; the `.hpp` is a VERBATIM copy of the profile-independent ERR struct defs. The old guard wrote
  each stub **only if it didn't already exist**, so after a `QuestStep` schema migration (new
  `progress_flag`/`entity_id` + the `quest_step_done` free function) the on-disk stubs stayed stale and
  **every non-ERR build broke** (compile error on the missing members, then link error on the missing
  `quest_step_done`) — and nothing regenerated them. Fix: refresh hpp/cpp on **content change**, and
  `synth_cpp` now also synthesizes **no-op definitions for free functions** declared in the hpp (regex
  for `<ret> <name>(<params>);` lines, emitted in `namespace goblin` returning false/0/nullptr). After
  any ERR-only schema change, re-run `py tools/gen_nonerr_stubs.py {erte,convergence,vanilla}`.

See [[mapforgoblins-pipeline-setup]], [[ghidra-worldmap-re]], [[quest-browser]].
