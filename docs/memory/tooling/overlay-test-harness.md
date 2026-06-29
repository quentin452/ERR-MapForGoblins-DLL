# Overlay test harness — AI-driven worldmap/minimap iteration (proposal)

**Status:** idea / proposed (2026-06-29). Goal raised by <user>: the worldmap + minimap are still
cluttered; a system that lets an AI **test and observe the overlay in real time** (an "interactive
Playwright" for the ImGui map) would make DX/clutter iteration far faster and deterministic.

## Is it possible? Yes — two complementary routes.

### Route A — Offline ImGui harness (recommended first; no game needed)
Build a small standalone exe that links the overlay's render code and feeds it **mock marker data**,
rendering ImGui to an **offscreen buffer** and dumping a PNG. Drive it with
[Dear ImGui Test Engine](https://github.com/ocornut/imgui_test_engine) for scripted UI actions
(open F1, type a search, toggle clustering, set marker-scale, pan/zoom) + screenshot capture.
- Deterministic: same mock data → same frame. No ER, no Proton, no live RE.
- AI loop: script action → screenshot → read PNG → assert/iterate on layout & clutter.
- Cost: factor the overlay draw out of the game-coupled bits (atlas load, projection) behind a
  data interface so mock data can feed it. Moderate refactor; high payoff for UX work.
- Limitation: tests layout/clutter/UX, NOT live game state (collected flags, real projection).

### Route B — In-game debug RPC + screenshot (for runtime behaviour)
Add a debug endpoint to the DLL (named pipe / loopback socket, dev-only, behind a config gate) that
accepts commands (`open_f1`, `search "<item>"`, `set_scale`, `toggle_cluster`, `screenshot`) and
returns the overlay framebuffer + state. An external Python driver gives the AI the same
script→observe loop against the **real running game**.
- Tests true runtime behaviour (live projection, collected state, the double-draw/cursor bugs).
- Cost: a command dispatcher + framebuffer grab in the Present hook. Reuses existing config plumbing.
- Caveat: needs Windows + a running ERR (Route B is a Windows-runtime tool, see [windows.md](../windows.md)).

## Recommendation
Do **Route A** for clutter/layout/DX iteration (deterministic, no game, AI-drivable today), and add
**Route B** later when a bug only reproduces at runtime (e.g. the non-deterministic double-draw in
[../bugs/dx-bugs-backlog](../bugs/dx-bugs-backlog.md) items 11-12). The two share nothing but the goal,
so they can land independently.

Cross-ref: [../bugs/dx-bugs-backlog](../bugs/dx-bugs-backlog.md), [minimap-future-feature](../features/minimap-future-feature.md).
