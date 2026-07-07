# Greybox render freeze on Windows — ROOT CAUSE = w2s camera-finder, NOT r3d's D3D12

**Status: root cause found 2026-07-07. The freeze is `goblin::w2s::find_cam_instance` scanning the whole
process address space on the PRESENT thread every frame — shared by r3d AND the ImGui box render. My
earlier "r3d D3D12 present stall" theory (below, struck) was WRONG; the user's "it's probably not r3d"
was right.**

## Symptom recap

Every attempt to render the objects.toml greybox froze the game:
- r3d backend: cube rendered, then **0 FPS** (freezes #1/#2).
- ImGui backend (no D3D12 at all): **hang** right after `objects realize` (freeze #3/#4), the present
  thread stalled (`frame=` stops; RPC times out) while a background scan thread kept logging for a few
  more seconds, then a force-quit → the recurring `eldenring.exe+0x1EB9999` teardown crash. My one-shot
  `[OBJECTS-RENDER]` log NEVER fired → the hang is INSIDE the render block, before the first draw.

Both backends have ONE thing in common: they call **`goblin::w2s::get_camera()` every present frame**.

## Root cause: `find_cam_instance()` full-address-space scan on the present thread

`goblin_w2s.cpp::find_cam_instance()` caches the camera instance in `s_hit`. On a cache MISS it
**`VirtualQuery`-walked the entire committed address space** (`lpMinimumApplicationAddress` →
`lpMaximumApplicationAddress`) in 8 MB windows, scanning every qword for the camera vtable
(`base + VT_CAMSET_RVA`), **in one present-frame call**. ER is a multi-GB process → a SINGLE pass takes
long enough to freeze the frame (and trip the freeze watchdog). The first `get_camera` call inside the
render block hung before the one-shot `[OBJECTS-RENDER]` log could fire.

## ★ CORRECTION 2026-07-07 (Ghidra RE, D:\ghidra_proj2) — the premise above was wrong on two counts

The earlier "the RVA is a wrong guess → never caches → rescans every frame" theory was NOT confirmed by
the RE. Both assumptions it rested on are false:

1. **`VT_CAMSET_RVA = 0x2a7f2b8` is CORRECT for this build.** `tools/ghidra/rtti_index.txt` (built from
   the running 2.6.2.0 exe) lists `GameRendCameraSet@GameRend@CS@@` vtable at **er+0x2a7f2b8** — exactly the
   mod's constant. `VIEW_FROM_HIT = 0xE0` (hit = GameRend+0x10, VIEW @ GameRend+0xF0) is likewise confirmed
   by the live Linux calibration AND by the Ghidra init `FUN_1406800f0`. So the scan was NOT failing to
   match a wrong vtable — a full sweep would have found the instance; it just took too long ON the present
   thread. (The "re-scans every frame" was an inference, not a measurement — the real defect is the single
   unbounded sweep.)
2. **GameRend is NOT an FD4Singleton — it has NO static slot.** The HANDOFF's planned fix ("AOB → GameRend
   singleton slot, one deref, zero scan") is **not achievable as written.** GameRend is allocated inside the
   game's task-step tree and referenced by POINTER from many owners, none of which is a static global:
   - the per-frame VIEW writer `FUN_140b019b0` holds it as `renderObj+0xE8`;
   - the InGameStep factory `FUN_140aec120`/`FUN_140aed820` stores it at `InGameStep_megaobj+0xB3628`, and
     the dtor `FUN_140aed380` reads it back from there;
   - the mega-object is `new`'d in `FUN_140aeaaa0`, stored in a parent `CSStepTask` member array — heap all
     the way up (EzChildStep / InGameStep / MoveMapStep / CSStepTask<TitleStep>), no `mov [rip+disp]` store
     of the instance anywhere.
   So reaching GameRend from a static would mean walking live task-manager pointers with non-AOB-able slot
   indices — worse than the scan. **A memory scan for the vtable is genuinely the simplest way to find the
   live instance.** Full VIEW-writer chain in `windows_world_to_screen_camera_re_findings.md`.

## Fix SHIPPED 2026-07-07 — time-boxed, resumable scan (keeps the proven VIEW read; kills the hang)

Rewrote `find_cam_instance(bool exhaustive=false)` in `goblin_w2s.cpp`:
- **Per-frame path (`draw_present`/`get_camera`):** scan for at most **2 ms per call**, then return 0 (no
  camera THIS frame → render just skips a frame) and **resume next frame** via a persistent cursor
  (`s_cursor`, resumes mid-region at 8 MB-window granularity). Once a valid hit is cached, every later frame
  is the cheap re-validate path (unchanged). So acquisition costs ~a few tenths of a second of skipped
  frames on first enable, then near-zero — never a stall.
- **Never-found degrades gracefully:** a completed sweep with no valid hit resets and **backs off 2 s**
  before the next sweep (a permanently-absent/rejected vtable = light periodic scan + no render, never a
  hang).
- **RPC `w2s_probe` path (`exhaustive=true`):** one call runs a full sweep (2 s cap so even the manual probe
  can't wedge; re-run `w2s_probe` if it times out mid-sweep). Present-thread only (single caller set) → the
  file-static scan state needs no locking.

Both builds green (`build-err` + `build-err-hotreload`, both DLLs link). Change is host-only.

## Current default (still OFF, pending ONE live confirm)

`goblin::objects` render stays **OFF by default** (`g_render_enabled=false`) until verified once in-world.
`objects realize` stores the boxes but nothing calls get_camera unless `objects render on`. **Next boot:**
`objects render on` then move around a placed box — it must draw as a greybox with NO freeze; `w2s_probe`
should report `ok w2s cam GameRend=… VIEW@+0xF0:` with a live matrix and (after `w2s_probe dot on`) the dot
should track the player. r3d benefits from the same finder (shared `get_camera`).

---

## (struck) earlier WRONG theory — "r3d D3D12 present stall"

The original text of this file blamed r3d's native-Windows D3D12 draw (upload-heap VB, barriers, swapchain
format). That was wrong: the ImGui backend (zero D3D12) froze identically, which exonerates the D3D12 path
and points at the shared `get_camera` scan. Kept as a lesson: when two very different render backends fail
the same way, suspect their shared dependency, not each backend.
