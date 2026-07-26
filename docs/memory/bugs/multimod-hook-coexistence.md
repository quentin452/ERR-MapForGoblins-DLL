# Multi-mod hook coexistence — MapForGoblins vs other overlay DLLs

Status: fixed 2026-07-26 (MapForGoblins side + ER-DeathCounter-Mod side). In-game retest with a third
overlay still pending.

## Report

Running MapForGoblins together with another overlay mod (EROverlay.dll) makes the OTHER mod's overlay
disappear / go unresponsive. The same was reported for ER-DeathCounter-Mod, and predicted for
MapForGoblins + ER-DeathCounter together — i.e. the bug follows *our* hooking style, not one specific
pairing.

## Root causes (three, independent)

### 1. Process-wide input blanking (the "unresponsive overlay" half)

While the F1 panel (or the fullscreen vmap) is up, the input hooks falsify what the game reads:

| hook | file | what it does while capture is active |
|---|---|---|
| `GetRawInputData` | `src/input/input_rawinput.cpp` | zeroes the mouse event, kills keys |
| `GetRawInputBuffer` | same | returns 0 buffered events |
| DI8 `GetDeviceState` | `src/input/input_directinput.cpp` | `memset(data, 0, cb)` |
| DI8 `GetDeviceData` | same | `*inout = 0` |
| `GetCursorPos` | `src/input/input_cursor.cpp` | reports a frozen screen-centre |
| `SetCursorPos` | same | swallows the call |

These are process-wide entry points. Another mod's ImGui overlay polling the same APIs got the falsified
data too: frozen cursor at screen centre, no clicks, no keys → looks dead/hidden.

`XInputGetState` (`input_gamepad.cpp`) already had the right shape — a `_ReturnAddress()` +
`goblin::self_module_range()` check so our own module reads real data. The other five did not.

**Fix**: `goblin::caller_is_game(const void *ret_addr)` (`src/goblin_crashdump.{hpp,cpp}`) — true when the
return address lies inside the host executable's image (resolved name-agnostically via
`GetModuleHandleW(nullptr)`, so it is not ER-specific). Every falsifying branch above is now gated on it.
Fails **open** (returns true) when the host range can't be resolved, so a lookup failure degrades to the
old blank-everyone behaviour rather than letting the game move under an open panel.

### 2. `MH_Uninitialize()` erases other mods' hooks

`modutils::deinitialize()` called `MH_Uninitialize()`. MinHook uninstalls by writing the **pristine
prologue bytes** back over the target. In a multi-mod process that is not "undo my hook", it is "erase
whatever is at the head of the chain": if another mod hooked the same `IDXGISwapChain::Present` slot after
us, its `JMP` is what gets wiped. Called from the `setup_mod()` error path in `dllmain.cpp` — i.e. while
the game keeps running.

**Fix**: `deinitialize()` no longer touches MinHook. Its only two callers are that error path and
`DLL_PROCESS_DETACH` at process exit, where the address space is going away anyway.

Same defect and same fix in ER-DeathCounter-Mod's `OverlayManager::shutdown()`
(`MH_DisableHook(MH_ALL_HOOKS)` + `MH_Uninitialize()`), plus that DLL now pins itself
(`GET_MODULE_HANDLE_EX_FLAG_PIN`) so its still-installed detours can never point into unmapped memory.

### 3. Unconditional WndProc restore

`uninstall_wndproc_hook()` did a bare `SetWindowLongPtrW(hwnd, GWLP_WNDPROC, o_orig_wndproc)`. Any mod
that subclassed **after** us stored our `hk_wndproc` as its "original"; restoring ours dropped it out of
the chain entirely.

**Fix**: only unlink when `GetWindowLongPtrW(GWLP_WNDPROC) == &hk_wndproc` (we are still head). Otherwise
log and stay installed — `hk_wndproc` forwards via `CallWindowProcW`, so remaining in the chain is inert.

## Hook-chain diagnostic (added)

Both mods now log, at hook-install time, the Present target address, their detour, their **trampoline**,
and the module that owns the trampoline:

```
[OVERLAY] hook chain: Present target=... detour=... trampoline=... owner=eldenring.exe
[DIAG INIT] Present hook enabled on ... | detour=... trampoline=... owner=MapForGoblins.dll
```

Reading it: `owner=eldenring.exe`/`dxgi.dll` ⇒ we are first in the chain. Another mod's DLL name ⇒ it
hooked before us and we call through it (correct). An overlay that draws once and then never again — the
`present_frames` frozen symptom — means the chain is broken, and these two lines say where.

## Related

- `docs/memory/bugs/overlay-input-unfocused-hooks.md` — the focus gate on the same hooks.
- `docs/memory/bugs/overlay-input-hook-freeze.md`.
