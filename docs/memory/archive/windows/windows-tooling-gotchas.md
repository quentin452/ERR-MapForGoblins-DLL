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

See [[mapforgoblins-pipeline-setup]], [[ghidra-worldmap-re]].
