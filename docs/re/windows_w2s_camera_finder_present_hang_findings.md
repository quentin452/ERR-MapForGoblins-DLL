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

## Root cause: `find_cam_instance()` full-address-space scan every frame

`goblin_w2s.cpp::find_cam_instance()` caches the camera instance in `s_hit`. On a cache MISS it
**`VirtualQuery`-walks the entire committed address space** (`lpMinimumApplicationAddress` →
`lpMaximumApplicationAddress`) in 8 MB windows, scanning every qword for the camera vtable
(`base + VT_CAMSET_RVA`). ER is a multi-GB process → this scan is very expensive, and it runs **on the
present thread**. If it never finds a hit, `s_hit` stays 0 and it **re-scans the whole space EVERY
frame** → the present thread stalls for seconds per frame = the freeze/hang.

**Why it never finds on this box:** `VT_CAMSET_RVA = 0x2a7f2b8` is a **static, UNVERIFIED Ghidra guess** —
`docs/re/windows_world_to_screen_camera_re_findings.md` says "static recon done; live-probe pending … not
yet runtime-verified". On the running exe **2.6.2.0** it is likely wrong (or the camera layout differs),
so the vtable is never matched → permanent per-frame full rescan.

r3d's world-anchored cube was "live-verified 2026-07-05" — on the **Linux/Proton** dev box, whose ER build
(and thus the camera vtable RVA) differs from this Windows 2.6.2.0 install. So get_camera worked THERE and
has never actually resolved on this Windows box.

## The two independent fixes (w2s Windows-hardening — the real next task)

1. **Make `find_cam_instance` present-thread-SAFE.** Never full-scan the address space on the present
   thread every frame. Options: rate-limit the rescan (attempt at most once every few seconds when
   uncached, return 0 in between → the render just doesn't draw, no hang); bound the scan to ER's main
   heap regions; or run the find on a background thread and have the present thread use only the cached
   result. Then a wrong RVA degrades to "no render", never a hang.
2. **Verify/fix `VT_CAMSET_RVA` for exe 2.6.2.0.** It is an unverified static guess. Confirm the camera
   vtable live (a bounded `w2s_probe` that reports the found instance + the VIEW matrix), or re-derive the
   RVA for this exe. `VIEW_FROM_HIT = 0xE0` (GameRend+0xF0) is the same doc's guess and needs the same
   check.

## Current mitigation (shipped)

`goblin::objects` render is **OFF by default** (`g_render_enabled=false`) — `objects realize` stores the
boxes but nothing calls get_camera, so the game is stable. Enable with `objects render on` only after the
w2s fixes above. r3d likewise stays off (`r3d 1`) — same get_camera dependency.

---

## (struck) earlier WRONG theory — "r3d D3D12 present stall"

The original text of this file blamed r3d's native-Windows D3D12 draw (upload-heap VB, barriers, swapchain
format). That was wrong: the ImGui backend (zero D3D12) froze identically, which exonerates the D3D12 path
and points at the shared `get_camera` scan. Kept as a lesson: when two very different render backends fail
the same way, suspect their shared dependency, not each backend.
