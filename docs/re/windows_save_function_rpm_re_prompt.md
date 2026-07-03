# Windows RE prompt — find the ER SAVE/serialize function via LIVE RPM (sidecar Phase 2)

**Run on the Windows box** (CE GUI + Ghidra + external ReadProcessMemory tools). Precedent: the
worldmap native-clip B3 was solved by a live external-RPM scan far faster than offline Ghidra
(`docs/re/worldmap_native_clip_b3_scaleform_re_findings.md`, `GfxScan.cs`). Do the same here.

## Goal

Find the function to hook so MapForGoblins can bracket a save ATOMICALLY:
`strip custom items → original_save() (serializes clean + writes) → reinject`.
Deliver: **RVA + patch-resilient AOB + call convention + proof that its entry runs BEFORE inventory
serialization** (so stripping at entry keeps the item out of the written `.sl2`/`.err`).

## Why the in-DLL approach failed (context — don't repeat these)

- Hooking **CreateFileW** and stripping there does NOT clean the save: empirically the item is already
  serialized when the file opens, and ER's frequent autosave re-dirties after reinject
  (`docs/plans/shadow_sidecar_save_plan.md` Phase 2, disproven 2026-07-03).
- An in-DLL stack walk from the CreateFileW save-write (`goblin::sidecar::probe_save_callstack`) mapped
  the WRITE chain but the OUTER save routine is **indirect/virtual-dispatched on a worker thread**, so a
  call-site (`E8 rel32`) walk can't reach its entry, and a CC-scan entry heuristic mislabeled it (an
  observer on the guess got 0 calls). Full detail: `docs/re/linux_save_function_re_findings.md`.

## What we already KNOW (feed these to CE/Ghidra)

Save WRITE chain (RVAs off imagebase 0x140000000; er_base is ASLR'd live — RVAs are stable):
- `0x240daa0` + `0x240c530` = recursive **BND4/sl2 write tree** (0x240daa0 recurses through 0x240c530),
  operating on an already-serialized buffer.
- `0x2412f60` / `0x24142e0` / `0x1ee5c10` = write helpers; `0x1fc0b70` = the low-level file open that
  calls CreateFileW.
- Above them: indirect-dispatched worker frames (very high live addresses = task thunks).

Save REQUEST + inventory (from the Hexinton all-in-one CT):
- **`GameMan+0xB42` = the save-request flag** — the CT's `saveRequest()` does `writeByte(GameMan+0xB42, 1)`.
  `GameMan` AOB: `48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3` (mov rax,[rip+disp]; disp@+3,
  instr len 7 → slot = match+7+disp; GameMan = *slot).
- Inventory chain: `GameDataMan` (AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3`) →
  `+0x8 = PlayerGameData` → `+0x2B0 = EquipGameData` (the item list the serializer reads).
- AddItemFunc / MapItemMan (grant/remove) already REd: `re_signatures.hpp` ADD_ITEM_FUNC + INVENTORY_ACCESSOR.

## Method — LIVE RPM / CE (pick the fastest that lands)

### A. CE "find what accesses" on `GameMan+0xB42` (most direct)
1. Attach CE to `eldenring.exe`. Resolve `GameMan` (AOB above) → compute `GameMan+0xB42`.
2. Right-click → **"Find out what WRITES this address"** → in-game **rest at a Site of Grace** (or quit to
   menu). The writer(s) = the save-REQUEST sites. Note their functions.
3. Right-click → **"Find out what ACCESSES/READS this address"** → rest again. The reader that, when the
   flag is set, DISPATCHES the save = the **save initiator**. This function's ENTRY runs before
   serialize+write. Record its RVA. Walk up one frame if the reader is just a per-frame poll — the CALLER
   that then runs serialize→write is the hook target.
4. In Ghidra, open that function; confirm it calls (directly or via a couple of frames) BOTH a routine
   that reads `EquipGameData` (serialize) AND the write tree (`0x240daa0`). That confirms the bracket.

### B. RTTI vtable scan via external RPM (the GfxScan.cs technique) — if A is noisy
1. External C# (or Python) tool: `ReadProcessMemory` over the module + heap; find RTTI
   `TypeDescriptor`s whose name contains `SaveLoad` / `CSSaveDataMan` / `SaveData` / `RequestSaveData` /
   `CSSaveLoadThread` (FromSoft classes are `CS::…`). From a matched TypeDescriptor → its
   `CompleteObjectLocator` → the **vtable**. The virtual save method in that vtable = the
   indirect-dispatched outer routine the in-DLL walk couldn't reach.
2. Cross-check: the vtable method's body should reach the known write tree `0x240daa0`.

### C. Confirm serialize ordering
Whatever function you pick, verify its ENTRY is pre-serialize: set a CE breakpoint at the function entry,
another on a READ of `EquipGameData` item bytes (inventory). Trigger a save — the entry BP must fire
BEFORE the inventory-read BP. That proves strip-at-entry keeps the item out of the buffer.

## Deliverables (write to `docs/re/windows_save_function_rpm_re_findings.md`)

1. The **save routine RVA** + a unique **AOB** (patch-resilient; verify unique in `.text`) → add as
   `SAVE_FN` in `re_signatures.hpp` (replace the current wrong guess).
2. **Call convention** (args in rcx/rdx/…; return) so the DLL can trampoline-hook it and forward.
3. **Proof of the pre-serialize ordering** (the BP-order test in C, or the Ghidra call-order).
4. Whether it fires **once per save** and is **save-specific** (not also load / unrelated) — so
   strip/reinject only run on saves. Note the thread it runs on (worker vs main).

The DLL side is ready: `goblin::sidecar::strip_items()` / `reinject_items()` (via `give_item ∓qty`) +
the `[items]` store exist and are gated behind `kItemStripReinjectWired=false`; hooking the found
function (strip at entry, reinject at exit) + flipping that flag completes Phase 2.
