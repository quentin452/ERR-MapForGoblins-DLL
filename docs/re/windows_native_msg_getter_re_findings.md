# Native message getter (`MsgRepositoryImp::GetMessage`) — RE findings

Status: **RESOLVED** (static + live-verified, 2026-06-30).
Answers the 5 questions in `windows_native_msg_getter_re_prompt.md`.

Tooling: Ghidra 12.1.2 headless via `pyghidra` against the existing `ER` project
(`D:\ghidra_proj2\ER.rep`, eldenring.exe), plus a read-only `ReadProcessMemory`
probe of the **live ERR process** (PID was 9524). Scripts live in the session
scratchpad (`pg_*.py` for static, `live_repo.py` for the live read).

Image base for all RVAs: `0x140000000`. Live exe verified to be the **same
version** as the Ghidra DB (the `MSG_REPOSITORY` AOB resolves to the identical
RVA `0xafdc7b` in both).

---

## TL;DR

The native getter is:

```c
// FUN_14266d3c0  @ RVA 0x266d3c0
wchar_t *GetMessage(MsgRepositoryImp *repo, uint32_t group, uint32_t fmgId, uint32_t msgId);
//   RCX=repo   EDX=group   R8D=fmgId   R9D=msgId
```

Decompiled body:

```c
wchar_t *GetMessage(repo, group, fmgId, msgId) {
    if (group < *(u32*)(repo + 0x10)                        // group < groupCount
     && fmgId < *(u32*)(repo + 0x14)                        // fmgId < fmgCount  (== the mod's count2)
     && (grp = *(void**)(*(void**)(repo + 0x08) + group*8)) // base_array[group]
     && (fmg = *(void**)(grp + fmgId*8)))                   // base_array[group][fmgId]
        return FmgFile_lookup(fmg, msgId);                  // FUN_14266dc90 (per-FMG entry search)
    return 0;                                               // miss -> NULL
}
```

It is **not virtual** (the `MsgRepositoryImp` vftable @ `0x2baa440` holds only a
destructor; single inheritance), which is why it can't be found via the vtable
and isn't reached through the singleton global — callers hold the repo pointer
already and call `GetMessage` directly. It was located by decompiling the menu
wrapper `FUN_140d10950`:

```c
wchar_t *getMenuCommonText(repo, msgId) {
    s = GetMessage(repo, 0, 0x7c, msgId);
    if (!s) s = GetMessage(repo, 0, 0x4c, msgId);
    return s ? s : L"?MenuCommon?";          // <- the "?PlaceName?"-style placeholder pattern
}
```

---

## Q1 — Address / signature / AOB

* **Function:** `FUN_14266d3c0`, **RVA `0x266d3c0`**.
* **Signature:** `wchar_t* (MsgRepositoryImp* repo, u32 group, u32 fmgId, u32 msgId)`,
  x64 fastcall (RCX, EDX, R8D, R9D), returns the wide string pointer in RAX.
* **Tail-calls** `FmgFile_lookup` = `FUN_14266dc90` (RVA `0x266dc90`), the per-FMG
  entry lookup (binary search over the FMG group table — the same structure the
  mod walks by hand in `copy_fmg_entries`).

### AOB (anchor on the interior, NOT the prologue)

Static prologue bytes:

```
3B 51 10 73 29 44 3B 41 14 73 23 48 8B 41 08 8B D2 48 8B 0C D0 48 85 C9
74 14 41 8B C0 48 8B 0C C1 48 85 C9 74 08 41 8B D1 E9 <rel32> 33 C0 C3
```

**Important:** in the live ERR process the first 5 bytes (`3B 51 10 73 29`) are
**overwritten by a MinHook `E9 <rel32>` trampoline** — see Q4. An AOB anchored on
the entry prologue therefore fails under ERR. Anchor on the stable interior and
back up 5 bytes to reach the entry:

```
AOB  (starts at entry+5):
44 3B 41 14 73 ?? 48 8B 41 08 8B D2 48 8B 0C D0 48 85 C9 74 ?? 41 8B C0
48 8B 0C C1 48 85 C9 74 ?? 41 8B D1 E9
entry = match - 5
```

(The two short-jump rel8s and the final `E9` rel32 are wildcarded as
version-variant; everything else is opcodes/ModRM/semantic offsets. Verify
uniqueness with the existing `[SIG]` health check before trusting it.)

---

## Q2 — Category-arg semantics

The `category` argument is **`fmgId` (R8D), and it is the SAME physical slot index
the mod already uses**, with **`group` (EDX) = 0**.

* `repo+0x08` is `base_array` — exactly the mod's `base_array = *(repo+0x08)`.
* `group` selects `base_array[group]`; the mod (and the whole game, e.g. the menu
  wrapper above) uses **group 0**.
* `fmgId` indexes `base_array[group][fmgId]` — i.e. the mod's `sub[slot]`.
* The bound `fmgId < *(repo+0x14)` is literally the mod's `slot < count2`
  (`count2 = *(repo+0x14)`), and the mod's `19 >= count2` guard is this check.

So the mapping is the identity the mod already encodes:
GoodsName=10, WeaponName=11, ProtectorName=12, AccessoryName=13, NpcName=18,
PlaceName=19, GemName=35/42, and the DLC slots 31x/41x. **No separate FMG-category
enum** — `fmgId` is the slot number.

---

## Q3 — Miss behavior

Returns **NULL** on any miss (bad bounds, null group array, null FMG, or
`FmgFile_lookup` not finding `msgId`). Callers substitute their own placeholder
(`L"?MenuCommon?"`, `L"?PlaceName?"`, …). No exceptions, no empty-string sentinel.

---

## Q4 — Layer merge (the crux) — answered by reading the live repo

**`GetMessage` does NOT merge DLC layers itself.** It is a single
`base_array[group][fmgId] -> FmgFile_lookup(msgId)` with no layer loop.

Live read of the running ERR repo (`live_repo.py`) settles how layering actually
works on ERR:

```
repo+0x10 groupCount = 1            <- ONE group; "groups" are NOT the DLC layers
repo+0x14 fmgCount   = 512
group[0]:
   slot  10 GoodsName        strings=8984      <- base slot already huge
   slot  11 WeaponName       strings=8624
   slot  19 PlaceName        strings=38249
   slot 319 GoodsName_dlc01  strings=1   (stub)
   slot 410 WeaponName_dlc02 strings=1   (stub)
   slot 419 GoodsName_dlc02  strings=1   (stub)   ...all DLC slots = 1-string stubs
```

**Under ERR the loader folds the DLC strings into the BASE slot and stubs out the
vanilla DLC slot numbers.** That is the entire `?PlaceName?` / `#ifdef
MFG_VANILLA` story:

* The mod's hand-walk of `{dlc02, dlc01, base}` slot numbers (e.g. `{419,319,10}`)
  is, under ERR, walking 1-string stub FMGs at the *vanilla* indices — pointless
  and the source of the v1.0.15 crash.
* ERR achieves the merge by **hooking `GetMessage` itself** (see below), so a
  single `GetMessage(repo, 0, base_slot, msgId)` returns base+DLC merged content.

### ERR hooks the native getter

Live entry bytes at `0x266d3c0`:

```
E9 1C 3C 96 FD  44 3B 41 14 73 23 ...      (E9 rel32 = jmp to base-0x2f01f, a trampoline page)
```

A 5-byte `E9` detour over the original prologue, trampoline allocated just below
the image — classic MinHook. It is **not** MapForGoblins (the mod's only MinHook
targets are D3D Present/ResizeBuffers/ExecuteCommandLists and DirectInput). It is
**ERR's own hook**, which is how ERR injects merged DLC names. Calling the
function entry therefore runs ERR's enhanced lookup and returns the merged result.

---

## Q5 — Thread / timing safety

`GetMessage` is a pure structure read guarded by its own bounds + null checks; it
takes no visible lock. The mod already reads this exact repo structure off the
build-worker thread today (`setup_messages`), so calling `GetMessage` from there
is no riskier — provided the singleton is non-null and the msgbnd is loaded
(the live read shows the base slots fully populated, so the existing
"repo resolved" gate is sufficient). Caveat: ERR's hook adds its own code on top;
keep the call after message load and behind the same readiness gate.

---

## Recommended change (kills the `#ifdef` + ~100 lines, crash-safe everywhere)

Replace `copy_fmg_entries` / `copy_fmg_layered` / the slot tables with direct
`GetMessage` calls. Because `GetMessage` bounds- and null-checks internally and
`FmgFile_lookup` validates the FMG, **no SEH guard and no `#ifdef MFG_VANILLA`
are needed** — every slot (including ERR's stubs) is safe to query:

```c
// resolve once: gm = (GetMessageFn)(scan(GETMESSAGE_AOB interior) - 5)
const wchar_t *resolve(repo, msgId, std::initializer_list<int> slots) {
    for (int slot : slots) {                      // e.g. {10, 319, 419} for Goods
        const wchar_t *s = gm(repo, 0, slot, msgId);
        if (s && s[0]) return s;                  // first non-empty wins
    }
    return nullptr;
}
```

* **ERR:** the base slot hits with merged base+DLC content; the stub DLC slots
  return null/empty and are skipped. DLC items resolve. ✅
* **Vanilla:** base slot covers base ids; DLC-only ids fall through to the real
  DLC slots. ✅
* No binder-index corruption is possible — the mod never indexes a slot's
  internals itself; ERR's shifted indices can't crash us because `GetMessage`
  owns the bounds checks.

Add `GETMESSAGE` to `re_signatures.hpp` (interior AOB, entry = match-5) and the
`[SIG]` health-check table.
