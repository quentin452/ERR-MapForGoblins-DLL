# Windows Agent Notes

Windows does normal repo work too; its special role is **runtime RE** with the live game and Windows-only
tooling. Prefer Windows for anything that needs a running `eldenring.exe`.

## Windows-only / Windows-preferred

- Ghidra (project reuse + `query.java`/`rtti_index`). → `tooling/ghidra-re-tooling.md`, `tooling/ghidra-worldmap-re.md`
- Cheat Engine / runtime validation against the live game.
- External Python `ReadProcessMemory` probes vs a running `eldenring.exe`. → `tooling/rpm-live-memory-tooling.md`
- Oodle-only extraction: DarkScript3 EMEVD/ESD, FFDEC, the pythonnet/.NET data pipeline, packed-file work.
  → `tooling/darkscript3-emevd-decompile.md`, `tooling/mapforgoblins-pipeline-setup.md`
- ER Console mod as a coordinate-readout tool. → `tooling/er-console-mod.md`

## Dev-box quirks

- Invoke `.bat` via the PowerShell tool (Bash `cmd.exe` only prints the banner).
- Each tool call gets a copy-on-write FS snapshot — create dirs in the same call / via Write.
- Custom redirects go stale; read the background task's own output. Keep big artifacts on `D:\`.
- Pass env-var paths with forward slashes. → `tooling/windows-tooling-gotchas.md`

## How to answer an RE prompt

1. Read the prompt in `docs/re/windows_*_prompt.md`.
2. Check current code and existing findings first (offsets are resolved live at init via
   `re_signatures.hpp` + `resolve_field_offset` — see `tooling/param-offset-source-of-truth.md`).
3. Prefer the reusable Ghidra/RPM helpers over one-off scripts.
4. Validate offsets with the 4-check recipe (`tooling/re-offset-validation.md`) — never ship one
   hand-derived from paramdef packing.
5. Return a findings doc in `docs/re/*_findings.md` with concrete offsets, AOBs, confidence, runtime
   evidence, and implementation notes. Mark failed paths explicitly so they aren't retried.

Windows is not the only place to change code: pure C++/Python/docs work that needs no live runtime RE
can be done on either platform.
