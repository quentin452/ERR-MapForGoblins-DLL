# Linux Agent Notes

Linux can do most project work: edit C++/Python/docs, inspect generated data, run normal repo tooling, cross-build with clang-cl/xwin when configured, and prepare deployable DLLs for Proton.

Linux-specific strengths:

- Fast repo editing and normal build/test automation.
- Proton/Wine log inspection.
- Cross-build and deploy flow documented in `archive/linux/mapforgoblins-linux-build.md`.
- Wine performance lessons, especially RPM-to-self costs, in `archive/linux/linux-rpm-walk-danger.md` and `archive/linux/clang-cl-seh-noinline.md`.

Linux limitations:

- Do not assume Linux can complete Ghidra/RPM runtime RE that needs live Windows debugging.
- Cheat Engine breakpoints under Wine have historically frozen the game.
- Oodle/Windows-only toolchains may block some data extraction paths.

When a task needs Windows:

- Ghidra project reuse, Cheat Engine, live `ReadProcessMemory` scripts, and runtime debugger validation should be handed to a Windows agent.
- Write or update `docs/re/windows_*_prompt.md` with the exact question, known offsets/AOBs, failed attempts, and expected deliverable.

Start here for detail:

- `archive/linux/MEMORY.md`
- `archive/linux/mapforgoblins-linux-build.md`
- `archive/linux/linux-rpm-walk-danger.md`
- `archive/linux/msbe-parser-supersedes-bake.md`
- `archive/linux/overlay-rendered-markers.md`
