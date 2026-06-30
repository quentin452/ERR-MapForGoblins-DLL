# RE prompt — Proton 11 F1-menu cursor lock (user32 cursor hooks bypassed)

Status: **RESOLVED 2026-07-01, merged to master (`fix/focus-message-cursor-lock`).** H3 was the whole
story — user-confirmed fixed via the exact alt-tab repro after the WM_SETFOCUS/WM_KILLFOCUS forwarding
fix. H1/H2 below were never confirmed and are now believed wrong for this repro (refuted by the
`[CURSORDIAG]` log data — see H3). Leaving H1/H2 in this doc only in case a *different* trigger
(no alt-tab involved) ever reproduces a similar-looking freeze — re-open with fresh `[CURSORDIAG]` data
against that specific case before assuming it's the same bug.

**Step 1 instrumentation DONE 2026-07-01 (`diag/cursor-hook-call-counters`).** The 5 detours log
`[CURSORDIAG] hooks/sec: set_cursor_pos=... clip_cursor=... get_cursor_pos=... raw_input_data=...
raw_input_buffer=...` once/sec while the panel is open (`src/goblin_overlay.cpp`).

## H3 — reliable repro found, root cause confirmed by code (not just log data)

**User's repro (2026-07-01), 100% reliable:** (1) open the F1 ImGui panel, (2) Alt+Tab away from the
game, (3) Alt+Tab back to refocus, (4) cursor is lost forever — no hover/click/move, matches the
original symptom exactly.

**This REFUTES H1/H2 as the explanation for this repro.** Both hypothesized "0 calls reaching user32 at
all" (compositor-level lock or win32u bypass). The `[CURSORDIAG]` log from this exact repro shows the
opposite: `clip_cursor`/`get_cursor_pos`/`raw_input_data` keep firing throughout, including during the
"lost forever" tail (e.g. `clip_cursor=11..27`, `get_cursor_pos=44..108` per second while stuck) — only
`set_cursor_pos` drops to 0 after the alt-tab-back, while the others keep going. The hooks are NOT being
bypassed; something downstream of them is wrong.

**Root cause, found by reading the actual WndProc + focus-gate code (`src/goblin_overlay.cpp`):**
- `g_show = g_user_show && fg;` (`:3076` area) — recomputed **once per frame** from a foreground-window
  check. Comment: "lose focus → hooks deactivate; regain → panel restores."
- `hk_wndproc` (`:1296` area) only forwards messages to `ImGui_ImplWin32_WndProcHandler` in two cases:
  unconditionally for every message when `g_show` is true, or — when `g_show` is false but
  `world_map_open()` is true — **only** `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP`. **`WM_SETFOCUS`
  and `WM_KILLFOCUS` were never forwarded in that second branch.**
- Alt+Tab away: `WM_KILLFOCUS` likely arrives while `g_show` is still true (one frame stale) → forwarded
  fine, ImGui correctly registers focus lost.
- Alt+Tab back: `WM_SETFOCUS` arrives from the OS essentially immediately on refocus — very plausibly
  **before** the next Present-hook call has re-run the `fg` check and flipped `g_show` back to true. If
  so, `WM_SETFOCUS` falls into the `world_map_open()`-only branch, which doesn't forward it →
  `io.AddFocusEvent(true)` is never called → ImGui's internal focus-lost state never clears →
  `ImGui_ImplWin32_UpdateMouseData()` (standard ImGui Win32 backend behavior) permanently stops writing
  the mouse position from then on. Cursor "lost forever" — independent of Proton/Wayland/win32u entirely,
  a pure message-forwarding gap in our own WndProc hook.

**Fix applied:** forward `WM_SETFOCUS`/`WM_KILLFOCUS` to `ImGui_ImplWin32_WndProcHandler`
**unconditionally**, before the `g_show` branch, so ImGui's internal focus tracking can never desync from
real OS focus regardless of our panel-visibility gating. Safe: `hk_wndproc` can't be invoked before
`ImGui::CreateContext()`/`ImGui_ImplWin32_Init()` (both run earlier in the same `init_imgui()` that
installs the WndProc hook), and double-forwarding the same focus message when `g_show` is also true is
idempotent (`io.AddFocusEvent` just sets a bool).

**VERIFIED 2026-07-01 — user confirmed fixed** using the exact repro (F1 → alt-tab → alt-tab back →
cursor responds normally now). Done.

## Symptom (original framing — H1/H2 below; see H3 above for what's now believed to actually explain
## the reliable alt-tab repro)

After switching ELDEN RING (ERR / MapForGoblins) from **Proton 8.0 → Proton 11.0-100**, pressing **F1**
opens the ImGui panel but the **mouse cursor is frozen at screen-centre** — ImGui can't be moved or
clicked. Worked perfectly on Proton 8.0. Session is **Wayland** (`XDG_SESSION_TYPE=wayland`).

## What we already know (don't re-derive)

- The overlay frees the cursor for the menu by hooking three user32 exports with **MinHook inline hooks**
  (real function addresses via `GetProcAddress`, so all callers are caught), in `src/goblin_overlay.cpp`:
  - `hk_set_cursor_pos` (`:1175`) — `if (g_show) return TRUE;` swallow ER's per-frame recenter-to-middle.
  - `hk_clip_cursor` (`:1180`) — `if (g_show) return o_clip_cursor(nullptr);` unclip while menu up.
  - `hk_get_cursor_pos` (`:1186`) — feed real pos to ImGui during `g_imgui_reading_cursor`, frozen pos to
    the game otherwise.
  - Plus `hk_get_raw_input_data` (`:1220`) / `hk_get_raw_input_buffer` — zero raw mouse to the game when
    `g_show`. Install site: `hook_u32(...)` `:3333-3353`.
- **All five hooks install successfully under Proton 11** — the log shows `[OVERLAY] user32!<fn> hook
  installed` for each, no `HOOK FAILED`. So this is NOT a hook-install failure.
- `g_show` is correctly `true` when the panel is up (the panel renders; `g_show = g_user_show && fg`,
  `:3057`). So the suppression branch is reached — but ER's cursor lock is not being released.
- Conclusion so far: under Proton 11 ER's mouse-capture (recenter + clip) **does not reach the user32
  exports we hook**, so the detours' `g_show` swallow/unclip never applies to the real lock.

## Two hypotheses — DISAMBIGUATE FIRST (the fix differs entirely)

**H1 — Compositor-level pointer lock (native Wayland).** Proton 9+/11 can use the native Wayland driver,
which implements relative-mouse via the Wayland **pointer-constraints (locked-pointer)** protocol. The
pointer is then locked by the COMPOSITOR, not by wine's user32 — **no win32 hook can release it.** Fix is
environment-only (force XWayland), code hook is useless.

**H2 — win32u / NtUser bypass (XWayland or Wayland).** Modern wine (10/11) moved the Win32 GUI syscall
layer into `win32u.dll`. ER's `user32!ClipCursor` / `SetCursorPos` forward to `win32u!NtUserClipCursor` /
`NtUserSetCursorPos`, and wine's INTERNAL relative-mouse recentering may call the `win32u` path directly,
never re-entering the user32 export we hooked. Fix = hook the `win32u` exports too.

### Disambiguation steps (run in order, cheap → decisive)

1. **Instrument the existing detours with call counters.** Add a `static std::atomic<int>` per detour
   (`hk_set_cursor_pos`, `hk_clip_cursor`, `hk_get_cursor_pos`, `hk_get_raw_input_data`) incremented on
   every call, and log the counts once/second (or on F1 toggle). Launch Proton 11, enter gameplay, open
   F1. Read the log:
   - If `hk_set_cursor_pos` / `hk_clip_cursor` **fire ~every frame** during gameplay but the cursor is
     still locked when `g_show` → the swallow isn't enough / something re-locks below us → lean H1 or a
     deeper win32u re-lock.
   - If they **never fire** (0 calls) while ER clearly captures the mouse → ER's capture bypasses user32
     entirely → strong **H2** (or H1). Proceed.
2. **Env toggle test (decides H1 vs H2):** set the ER launch option `PROTON_ENABLE_WAYLAND=0 %command%`
   (force XWayland) and relaunch.
   - Cursor now frees in the F1 menu → it was **H1** (native Wayland pointer-lock). DONE — no code change;
     document the launch option as the fix and stop. (Also worth testing the inverse if already XWayland:
     confirm whether Proton 11 defaulted to native Wayland here.)
   - Still locked under XWayland → **H2** (win32u bypass). Build the code hook below.
3. (Optional, confirm H2 target) `WINEDEBUG=+cursor,+rawinput` on the ER prefix and grep the wine log for
   which API path does the recenter/clip (look for `NtUserClipCursor` / `NtUserSetCursorPos` /
   `set_cursor_pos` server calls vs `ClipCursor`).

## Code fix scope — ONLY if H2 confirmed (win32u bypass)

Mirror the user32 cursor hooks onto `win32u.dll`. Under wine, `win32u` exports are **real C functions**
(not the 5-byte syscall stubs they are on Windows), so MinHook can trampoline them — feasible under
Proton, unlike native Windows.

- **Targets** (resolve via `GetModuleHandleW(L"win32u.dll")` + `GetProcAddress`):
  - `NtUserSetCursorPos(INT x, INT y)` — detour: `if (g_show) return TRUE;` (mirror `hk_set_cursor_pos`).
  - `NtUserClipCursor(const RECT *rc)` — detour: `if (g_show) return o_nt_clip_cursor(nullptr);`.
  - `NtUserGetCursorPos` if present (some wine versions: `NtUserGetCursorInfo`) — mirror
    `hk_get_cursor_pos` (real pos to ImGui under `g_imgui_reading_cursor`, frozen to game otherwise).
  - Check for `NtUserGetRawInputData` / `NtUserGetRawInputBuffer` too if the raw-input zeroing also
    regressed (test whether raw deltas still reach the game with the menu open).
- **Install:** extend the `hook_u32` lambda (`:3333`) into a generic `hook_export(module, name, detour,
  orig)`; call it for both `user32` and `win32u`. Tolerate a missing `win32u` export (log + skip) so the
  build still works on older Proton / Windows.
- **Double-fire guard:** `user32!ClipCursor` forwards to `win32u!NtUserClipCursor`, so a single ER
  `ClipCursor` call could hit BOTH detours. The detours are idempotent under `g_show` (both just
  swallow/unclip), so double-firing is harmless for the swallow path — but verify `hk_get_cursor_pos`'s
  frozen-vs-real logic doesn't get confused when both layers fire in one frame (the `g_imgui_reading_cursor`
  window is set around `ImGui_ImplWin32_NewFrame`, `:3120-3123`, so it should still gate correctly). If it
  does double-count, prefer hooking ONLY `win32u` and dropping the `user32` cursor hooks on Proton 11.
- **Keep it mod/version-agnostic** (prime directive): the win32u hooks are ADDITIVE and must no-op
  cleanly when the export is absent (native Windows, old Proton). Never make win32u the only path on a
  build that might run on Windows.

## Acceptance test

Proton 11, Wayland session, in active gameplay (mouse captured for camera): press F1 → the OS cursor moves
freely over the ImGui panel, hover highlights work, clicks land on the control under the pointer (not at
screen-centre), the search `InputText` takes focus on click. Close F1 → camera mouse-look resumes, cursor
re-hidden/clipped (no leaked OS cursor). Re-verify on Proton 8.0/9.0 (no regression) and confirm the
`user32`-only path still works where `win32u` is absent.

## Pointers

- `src/goblin_overlay.cpp`: cursor/raw-input detours `:1175-1289`; free-cursor + re-clip-on-close logic
  `:3040-3090`; `g_imgui_reading_cursor` window `:3112-3130`; hook install `hook_u32` `:3333-3353`.
- Related (different cause, don't conflate): `docs/re/windows_input_softlock_re_prompt.md` (Deskflow
  screen-edge latch + the F1 mouse-dead half).
- Workaround until fixed: run ER on **Proton 9.0 / GE-Proton10-34** (user32 cursor path intact) instead of
  11, or `PROTON_ENABLE_WAYLAND=0 %command%` if H1.
