# Windows Agent Notes

Windows can do normal repo work too. Its special role is runtime RE with the game and Windows tooling.

Windows-only or Windows-preferred work:

- Ghidra project reuse and scripts.
- Cheat Engine/runtime validation.
- Python `ReadProcessMemory` probes against a running `eldenring.exe`.
- Windows-only extraction/tooling such as DarkScript3, FFDEC, pythonnet/.NET pipeline paths, and packed-game file validation.

Important Windows archive notes:

- `archive/windows/ghidra-re-tooling.md`: reusable Ghidra workflow.
- `archive/windows/ghidra-worldmap-re.md`: world-map/menu RE history.
- `archive/windows/rpm-live-memory-tooling.md`: Python RPM workflow.
- `archive/windows/windows-tooling-gotchas.md`: shell/path/tool quirks.
- `archive/windows/mapforgoblins-pipeline-setup.md`: local pipeline setup.

How to answer RE prompts:

- Read the prompt in `docs/re/windows_*_prompt.md`.
- Check current code and existing findings before running new analysis.
- Prefer reusable Ghidra/RPM helpers over one-off scripts when possible.
- Return a findings doc in `docs/re/*_findings.md` with concrete offsets, AOBs, confidence, runtime evidence, and implementation notes.
- Mark failed paths explicitly so future agents do not repeat them.

Windows is not the only place to make code changes. If a task is just C++/Python/docs and does not require live runtime RE, either platform can do it.
