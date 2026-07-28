# RE findings — EMEVD condition-group evaluator (native logic oracle)

Answers `docs/re/windows_emevd_condition_evaluator_re_prompt.md`.
**Route B (static, Windows Ghidra)** — project `D:\ghidra_proj2\ER`, program `eldenring.exe`,
app **2.6.2.0** (imagebase `0x140000000`), driven headless via `pyghidra` (no re-analysis).
**Nothing was measured live** — the game was not running this session (see §0.1). Every address
below is a **static** RVA on 2.6.2.0 and carries the AOB that resolves it.

---

## 0. TL;DR

- **T1 FOUND — the EMEVD instruction dispatcher is `FUN_140567d40` (rva `0x567d40`).** It is a
  three-way `switch` on the **EMEVD instruction bank** (`0..20`, `1000..1020`, `2000..2020`) read
  from the event instance's own instruction cursor. Each case calls a per-bank handler which
  `switch`es again on the **instruction index within the bank**. This is the real (bank, index)
  dispatch the prompt predicted.
- **T2 FOUND — conditions are first-class heap objects with a virtual `Evaluate`.** Base class
  `CSEmkCondition` (RTTI vtable rva `0x2a5acf0`); **`Evaluate` is vtable slot 1**. The
  per-event-instance **condition-group container is `CSEmkEventIns + 0x40`** (a singly-linked list
  keyed by the group index). The wiring the static scan is *guessing at* is explicit in the code:
  every condition instruction ends with `SetConditionGroup(this, <group from arg[0]>, <cond obj>)`.
- **T3 FOUND — the item test is `FUN_14057c8e0` (rva `0x57c8e0`)**, reached from bank-3 case 4 and
  case 16 (EMEDF `3[4] IfPlayerHasdoesntHaveItem`, `3[16]` = the include-storage variant). It is a
  **condition factory** (allocates a `0x38`-byte condition object), not a bare predicate — see §5,
  this matters for G2.
- **G1 = YES** (statically; needs one live confirmation). One hook at `0x567d40` yields
  `(event instance, bank, index, raw operand blob)` for **every EMEVD instruction executed**, and
  the condition group index is `arg[0]` of every condition instruction. See §6.
- **G2 = NO, as the prompt expected.** Conditions are stateful heap objects owned by a live event
  instance and linked into that instance's group list; there is no re-entrant
  `Evaluate(state) -> bool` that can be called on a synthesized state. See §7.

### 0.1 Capability triage (done first, as instructed)

| Check | Result |
|---|---|
| Elden Ring running + debug RPC | **NO.** `127.0.0.1:38700` connect timed out; no `eldenring.exe` in `tasklist`. Game **not booted** (not booted by me either — read-only session). Freshness gate (`mfg_build`) therefore not run: no live DLL to gate. |
| Ghidra install | **YES** — `D:\ghidra\ghidra_12.1.2_PUBLIC` |
| Ghidra project for `eldenring.exe` | **YES** — `D:\ghidra_proj2\ER.gpr` / `ER.rep` (~2.8 GB, fully analyzed, program `eldenring.exe`). Also `D:\ghidra_proj_ergg` = the ERR loader DLL only, not the game. |
| `rtti_index.txt` | **YES** — `D:\ghidra_scripts\rtti_index.txt`, 10202 vtables |
| Route chosen | **Route B (static)**. Route A unavailable. |

Raw run artifacts (gitignored scratch, on this box only):
`D:\ghidra_scripts_mfg\out_emevd_1.txt` … `out_emevd_6.txt`.

---

## 1. Entry point — `IsEventFlag` and its callers

`IS_EVENT_FLAG` (`src/re_signatures.hpp:25`, `48 83 EC 28 8B 12 85 D2`) resolves **uniquely** in
`.text` to **rva `0x5d1330`** = `FUN_1405d1330`. This agrees with
`windows_collected_loot_flag_re_findings.md`, which independently pinned the same function on the
same 2.6.2.0 DB — good cross-check that the DB and the AOB are both still current.

It has **151 xrefs / ~120 distinct caller functions** — far too many to classify by eye, and most are
UI/menu/quest code, exactly as the prompt warned. The discriminator that worked was **not** the
caller count but **code locality**: the RTTI index puts the whole EMEVD kernel ("Emk") in
`0x560000–0x590000`, and one caller inside it — `FUN_14057ef10` with **4** call sites — turned out
to be the flag-range condition (§3).

**RTTI classes that name the subsystem** (from `rtti_index.txt`):

| Class | vtable rva |
|---|---|
| `CSEmkCondition` | `0x2a5acf0` |
| `CSEmkEventIns` | `0x2a5dbb0` |
| `CSEventIns` | `0x2a5dde0` |
| `CSEventManImp` | `0x2a6f9a8` |
| `CSEmevdRepositoryImp` | `0x2ba32f0` |
| `CSEmkResManImp` | `0x2ba2748` |

---

## 2. T1 — the instruction dispatcher `FUN_140567d40` (rva `0x567d40`)

Reached bottom-up: `0x57ef10` ← `0x57ed60` ← `0x56ef00` ← **`0x567d40`** ← `0x582d50`
(= `CSEmkEventIns` **vtable slot 1**, the per-tick step).

```c
// FUN_140567d40(bankHandlerArray* param_1, ?, CSEmkEventIns* param_3)
pcVar3 = *(char **)(param_3 + 0xd0);   // current instruction descriptor
uVar1  = *(uint *)pcVar3;              // <-- EMEVD BANK
if (1000 < uVar1) { ... switch(uVar1) { case 0x3e9: FUN_140575d90(param_1[1]); ... } }
if (uVar1 == 1000) { ... }
if (uVar1 <  0x15) { switch(uVar1) { case 0: ... case 0x14: ... } }
LAB_14056806f:
  if (bVar2 != 0) FUN_140584180(param_3);   // instruction satisfied -> advance the cursor
  return bVar2;
```

The three switch ranges are **exactly the EMEVD bank numbering**:
`0..0x14` (0–20), `1000`, `0x3e9..0x3fc` (1001–1020), `2000`, `0x7d1..0x7e4` (2001–2020).
`param_1` is a **16-entry array of per-bank handler objects**; `param_1[n]` is passed to the bank
handler, so bank→handler is data-driven, not hardcoded per call.

Confirmed bank→handler mapping for the banks that matter to the randomizer:

| Bank | Handler rva | Note |
|---|---|---|
| 3 | `0x56e090` | the `IF …` condition bank (incl. the item test) |
| 4 | `0x56a880` | |
| 5 | `0x571ce0` | |
| 1000 | `0x574ed0` | control flow |
| 1003 | `0x56ef00` | event-flag conditions / skips |
| 1005 | `0x572180` | |
| 2000 | `0x5748b0` | |
| 2005 | `0x571060` | |

**`CSEmkEventIns` field offsets read out of the dispatch code** (all confirmed by two independent
users, `0x567d40` and `0x56ef00`, which index the same object as `param_3+0xNN` and `param_3[0xNN/8]`):

| Offset | Meaning |
|---|---|
| `+0x30` | argument / entity-id resolution context (`FUN_140583460(this+0x30, …)`) |
| `+0x40` | **condition-group container** (see §4) |
| `+0xC8` | EMEVD resource/repository handle (used to fault in the arg blob) |
| `+0xD0` | current instruction descriptor: `u32 bank` @+0, `u32 index` @+4, arg-blob offset @+0x10 |
| `+0xD8` | cached pointer to the instruction's **raw operand blob** (lazily filled from `+0xC8`) |
| `+0xE0` | skip/jump counter written by `SkipIf…` instructions |

`FUN_140584180` (rva `0x584180`) advances the instruction cursor. **No unique AOB found** for it at
16/20/24/32/40 bytes — it is not a good anchor; use `0x567d40` instead.

`FUN_140582d50` (the step) also does
`*(CSEmkEventIns **)(DAT_143d67bd0 + 0x120) = this;` — i.e. there is a **global "currently executing
EMEVD event instance"** slot at `[<CSEmkSystem singleton @ rva 0x3d67bd0>] + 0x120`. Useful as a
cheap secondary read, but the dispatcher argument is the primary.

---

## 3. The flag condition — `FUN_14057ef10` (rva `0x57ef10`)

This is a **condition object's `Evaluate`**, not a free function: `param_1` is the condition object.

```c
uVar5 = *(uint *)(param_1 + 0x2c);          // flag id range LOW
if (*(uint *)(param_1 + 0x30) < uVar5) { *(byte*)(param_1+0x18) &= 0xfe; return; }  // empty range
cVar2 = *(char *)(param_1 + 0x28);          // logical operation
// cVar2 == 0 : AND  of IsEventFlag(id)        (all ON)
// cVar2 == 1 : AND  of !IsEventFlag(id)       (all OFF)
// cVar2 == 2 : OR   of IsEventFlag(id)        (any ON)
// cVar2 == 3 : OR   of !IsEventFlag(id)       (any OFF)
...
bVar3 = FUN_1405d1330(DAT_143d68448, local_res8, ...);   // IsEventFlag(EventFlagMan, &id)
*(byte *)(param_1 + 0x18) = (*(byte*)(param_1+0x18) & 0xfe) | bVar6;   // <- RESULT, bit0
```

`DAT_143d68448` (rva `0x3d68448`, 330 refs) is the `EventFlagMan` singleton slot.

**This is precisely the semantics `tools/_probe_flag_gates.py` reimplements** — the 4-way
`LogicalOperationType` over an inclusive `[lo, hi]` flag-id range, with `id == 0xFFFFFFFF` normalised
to `0`. Condition-object layout, read here:

| Offset | Meaning |
|---|---|
| `+0x00` | (i8) condition-group index — the key `0x581f90` matches on (§4) |
| `+0x10` | next-condition pointer (intrusive list link) |
| `+0x18` bit0 | **evaluated result** |
| `+0x28` | (i8) logical operation 0=AllOn 1=AllOff 2=AnyOn 3=AnyOff |
| `+0x2C` / `+0x30` | (u32) flag id range low / high (inclusive) |

⚠️ `+0x00` as the group index is **inferred**, not proven: `0x581f90` compares its `group` argument
against `*(char*)cond` while walking the list, but the store of that byte was not located in this
pass (it is not in `0x581f10`). Verify before relying on it — see §8.

---

## 4. T2 — the condition-group container (`CSEmkEventIns + 0x40`)

Three small functions define it:

- **`FUN_140583820` (rva `0x583820`) — "attach condition to group".** Called at the end of *every*
  condition instruction as `FUN_140583820(this, <group>, <condObj>)`; it forwards to
  `FUN_140581f10(this + 0x40, group, condObj)`.
- **`FUN_140581f10` (rva `0x581f10`)** links the condition into the container: `+0x18` = list head,
  `+0x20` = list tail, `cond+0x10` = next. Group `0` (MAIN) is *also* cached at container `+0x10`.
- **`FUN_140581f90` (rva `0x581f90`) — "evaluate group G".**

```c
// FUN_140581f90(container, char group)
pcVar1 = *param_1;                       // list head
while (pcVar1) { if (group == *pcVar1) break; pcVar1 = *(char **)(pcVar1 + 0x10); }
if (!pcVar1) return 0;
return FUN_140581cc0();                  // -> virtual Evaluate on the found condition
```

`FUN_140582020` (rva `0x582020`) reads the **MAIN** group (container `+0x10`) and returns a
tri-state (`0` / `1` / `2`), which the step function `0x582d50` branches on.

**`Evaluate` is vtable slot 1.** Proven by the `.rdata` xref to `0x57ef10`: the pointer sits at
rva `0x2a5ce40`, in a 4-slot vtable starting at rva **`0x2a5ce38`** →

```
[0] 0x57ed10   (ctor/dtor-ish)
[1] 0x57ef10   <-- Evaluate  (the flag-range condition)
[2] 0x57eef0   thunk
[3] 0x57ef00   thunk
```

which lines up with the `CSEmkCondition` **base** vtable (rva `0x2a5acf0`) whose slot 1 is
`0x581b70` (the base/no-op `Evaluate`). So **every** condition type — flag, item, region, HP,
elapsed-time — is evaluated through one uniform virtual at slot 1.

---

## 5. T3 — the item test `FUN_14057c8e0` (rva `0x57c8e0`)

Bank-3 handler `FUN_14056e090`, case 4:

```c
case 4:
  puVar9 = *(u8 **)(param_3 + 0xd8);                 // operand blob (faulted in if null)
  uVar12 = *puVar9;                                  // arg[0] = condition group  (i8)
  uVar10 = FUN_14057c8e0(puVar9[1],                  // arg[1] = item type        (u8)
                         *(u32 *)(puVar9 + 4),       // arg[4] = item id          (i32)
                         puVar9[8] != '\0',          // arg[8] = owner state      (u8)
                         0);                         // includeStorage = 0
  break;
...
FUN_140583820(param_3, uVar12, uVar10);              // attach to condition group
```

Case `0x10` (16) is byte-identical except the trailing constant is `1` — the **include-storage**
variant. The operand layout matches EMEDF `3[4] IfPlayerHasdoesntHaveItem(sbyte conditionGroup,
byte itemType, int itemId, byte ownerState)` **exactly**, byte offset for byte offset, on the live
binary. That is a direct, mod-agnostic confirmation of the item-axis operand encoding the
randomizer's static scan currently assumes.

**Important nuance for G2:** `FUN_14057c8e0` decompiles to a tail-call into `FUN_141eb9ed0(0x38)`
(an allocator) — it **constructs** a `0x38`-byte condition object rather than returning a bool. The
same is true of `FUN_14057b020` (`0x30` bytes). So the bank handlers *build* conditions; the actual
truth value only materialises later, inside slot-1 `Evaluate`, against the live event instance.

---

## 6. G1 verdict — **YES** (static; one live confirmation outstanding)

> *Can we HOOK the evaluator and passively log `(event id, condition group, instruction, operands,
> result)` while a human plays normally?*

Yes, and it needs **one** hook, not a table of them.

**Hook `FUN_140567d40` (rva `0x567d40`)** — the bank dispatcher. At entry it already has everything:

| Wanted | Where, at hook entry |
|---|---|
| event instance | `rdx`-side arg `param_3` (also `[CSEmkSystem + 0x120]`) |
| **instruction** (bank, index) | `*(u32*)([param_3+0xD0])`, `*(u32*)([param_3+0xD0]+4)` |
| **operands** (raw blob) | `[param_3+0xD8]` (fault it in the same way the handlers do if null) |
| **condition group** | `arg[0]` of the blob — uniform across every condition instruction (§5) |
| **result** | the function's own `bool` return — log on exit |

That yields a per-instruction trace with **causation, not co-occurrence**: the group index is read,
not inferred, so "which flag test feeds which group, and which group gated the action that fired"
stops being a guess. It is mod-agnostic by construction — it observes whatever EMEVD the loaded mod
shipped, with no parsing on our side.

Two refinements, both optional:
- Hooking condition **slot-1 `Evaluate`** instead gives per-tick truth values, but requires
  hooking N subclass vtables; the dispatcher hook is strictly cheaper and already sufficient.
- `FUN_140581f90` (`0x581f90`) is the natural place to log *group* results if per-tick group state
  is wanted.

**Cost warning:** this fires for every instruction of every live event, every tick. It must be a
ring buffer / sampled writer, not a synchronous log line, and it must be dev-only.

**Not yet confirmed live.** The chain is fully static. Before building on it, run the live checks
in §8 — the AOBs are proposed, not runtime-verified.

## 7. G2 verdict — **NO** (documented, as the prompt allowed)

> *Can the evaluator be CALLED on a synthesized state ("in this state, is group G true?") without a
> live event instance?*

**No, and it should not be forced.** Three independent reasons, all visible in the code:

1. **There is no `Evaluate(state) -> bool`.** Slot-1 `Evaluate` takes only `this`; it writes its
   answer into the condition object (`+0x18` bit0) and reads the *global* live world
   (`IsEventFlag(EventFlagMan, …)`, inventory, region occupancy). There is no state parameter to
   synthesize into.
2. **Conditions cannot be built standalone.** The factories (`0x57c8e0`, `0x57b020`) heap-allocate
   and the results are immediately linked into a specific `CSEmkEventIns`'s group list
   (`0x581f10` writes head/tail/next). Building one without an owning instance means hand-rolling
   the object and leaking or corrupting the list.
3. **It would answer the wrong question anyway.** Even if called, `Evaluate` reads the *current*
   world, so it can only ever answer "is G true **now**" — never "would G be true in hypothetical
   state S". The randomizer's question is counterfactual; this oracle is not.

The useful capability is therefore **observation (G1), not interrogation (G2)** — which is exactly
the "oracle, not planner" framing the prompt opened with.

---

## 8. Proposed `re_signatures.hpp` block — NOT wired in

Static-unique in `.text` on **2.6.2.0** (each verified: exactly one match image-wide at the length
shown). **Not runtime-verified — do not wire in before the live check below.**

```cpp
    // ── EMEVD VM (event-script interpreter) — dev-only observation, see
    //    docs/re/windows_emevd_condition_evaluator_re_findings.md ──
    // Instruction dispatcher: switch(bank) -> per-bank handler, per EMEVD instruction.
    // (this = CSEmkEventIns; +0xD0 instr descriptor {u32 bank, u32 index}, +0xD8 operand blob,
    //  +0x40 condition-group container.) THE hook point for a passive EMEVD trace.
    inline constexpr const char *EMEVD_DISPATCH =
        "48 89 5C 24 08 57 48 83 EC 20 49 8B 80 D0 00 00";           // rva 0x567d40
    // Bank 3 handler ("IF …" conditions, incl. 3[4]/3[16] player-has-item).
    inline constexpr const char *EMEVD_BANK3_HANDLER =
        "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 49"; // rva 0x56e090
    // Bank 1003 handler (event-flag conditions / SkipIf).
    inline constexpr const char *EMEVD_BANK1003_HANDLER =
        "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 49"; // rva 0x56ef00
    // Condition "attach to group": (CSEmkEventIns*, i8 group, CSEmkCondition*).
    inline constexpr const char *EMEVD_SET_COND_GROUP =
        "48 8B C4 41 56 48 83 EC 40 48 C7 40 D8 FE FF FF";           // rva 0x583820
    // Condition-group container: link a condition into the group list.
    inline constexpr const char *EMEVD_COND_ATTACH =
        "4D 85 C0 74 6E 48 89 5C 24 10 48 89 6C 24 18 57";           // rva 0x581f10
    // Condition-group container: evaluate group G (walks list, matches group byte, virt-calls slot 1).
    inline constexpr const char *EMEVD_COND_EVAL_GROUP =
        "48 8B 09 48 85 C9 74 0D 3A 11 74 0C 48 8B 49 10";           // rva 0x581f90
    // "IF Event Flag Range" condition Evaluate (vtable slot 1 of its subclass, vt rva 0x2a5ce38).
    // Layout: +0x18 bit0 result, +0x28 logical-op(0=AllOn 1=AllOff 2=AnyOn 3=AnyOff), +0x2C/+0x30 id range.
    inline constexpr const char *EMEVD_COND_FLAG_RANGE_EVAL =
        "40 53 57 41 57 48 83 EC 30 8B 59 2C 4D 8B F8 48";           // rva 0x57ef10
    // Item-possession condition factory: (u8 itemType, i32 itemId, bool ownerState, bool inclStorage).
    inline constexpr const char *EMEVD_HAS_ITEM_COND =
        "40 57 48 83 EC 50 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 48 89 74 24 70 "
        "41 0F B6 D9 41 0F B6 F8 8B F2";                              // rva 0x57c8e0
```

⚠️ Two of these are **thin** and should be re-checked after any game patch:
`EMEVD_BANK3_HANDLER` and `EMEVD_BANK1003_HANDLER` share a very common MSVC prologue and are only
disambiguated by the trailing `48 83 EC <frame>` byte — a frame-size change on either would make
them collide. `EMEVD_COND_EVAL_GROUP` is a mid-function-style byte run and is fine, but short.
`EMEVD_IP_ADVANCE` (rva `0x584180`) was **rejected**: no unique signature at up to 40 bytes.

---

## 9. Next concrete step (one thing) — ✅ DONE 2026-07-28, see §10

**Live-verify `EMEVD_DISPATCH` (`0x567d40`) with a read-only RPC probe, before any hook is written.**

1. Boot ER, get in-world, run the freshness gate (`mfg.py rpc mfg_build`, then `status`), record
   `er_version` + `er_base`.
2. Resolve `EMEVD_DISPATCH` with the existing signature machinery and check it lands at
   `er_base + 0x567d40` on 2.6.2.0 (drift is fine — the AOB is the contract, the RVA is only a
   sanity check).
3. Arm `mem_fwa` on the resolved entry and confirm it fires continuously in-world and **stops at
   the main menu** — that is the cheap proof it is the live EMEVD VM and not a look-alike.
4. Then, and only then, read `[this+0xD0]` / `[this+0xD8]` at a few hits (`mem_dump`) and check the
   bank values fall in `{0..20, 1000..1020, 2000..2020}`. If they do, G1 is confirmed live and the
   passive tracer can be scoped.

Open item to close during that pass: **prove the condition-group byte at `cond+0x00`** (§3) by
dumping a condition object attached via `0x583820` and comparing byte 0 against the `arg[0]` the
dispatcher saw. That is the one inferred field in this whole chain.

---

## 10. LIVE VERIFICATION — 2026-07-28 (Windows box)

Everything above §9 was static. This section is **measured on a running game**.

**Setup.** VANILLA Elden Ring (`E:\SteamLibrary\steamapps\common\ELDEN RING\Game\eldenring.exe`,
UXM-unpacked), `MapForGoblins.dll` loaded from `C:\Users\iamacat\Downloads\DLLS\` (built
`Jul 27 2026 16:13:27`), NOT the ERR install. `er_version = 2.6.2.0` — **the same build the Ghidra DB
holds**. `er_base = 0x7ff675600000`. In-world (`coords` → `area=60`, Limgrave), not the main menu.
Note this lands the confirmation on **vanilla**, which is the stronger side for the mod-agnostic claim.

### 10.1 ⚠ Correction to §9's method — `mem_fwa` cannot watch CODE

§9 proposed "arm `mem_fwa` on the resolved dispatcher entry". **That cannot work.** `mem_fwa` sets a
DR0 **data** breakpoint: `make_dr7()` emits `RW = 0b11` (read|write) or `0b01` (write) and never
`0b00` (execute) — `src/goblin_field_probe.cpp:57`. An x86 data watchpoint does not trigger on
instruction fetch, so arming the dispatcher entry would sit silent forever and read as a false
negative.

**What was used instead** — the per-step WRITE this doc already identified in §2:
`*(CSEmkEventIns **)(DAT_143d67bd0 + 0x120) = this`. That is a data write, every step, so it is
exactly what a data watchpoint is for.

### 10.2 The dispatcher AOB resolves, byte for byte

```
rpc mem_dump 0x7ff675b67d40 16        # = er_base + 0x567d40
ok 0x7ff675b67d40: 48 89 5c 24 08 57 48 83 ec 20 49 8b 80 d0 00 00
```

Identical to `EMEVD_DISPATCH` (§8). The RVA and the AOB both hold on the live 2.6.2.0 image.

### 10.3 The `+0x120` set/clear pair, confirmed at instruction level

`mem_fwa <CSEmkSystem+0x120> 8 w` fired **6 ms after arming**, standing still in Limgrave — this is a
continuous, high-frequency site, not a rare one. Two hits, decoded from the logged byte windows:

| Hit | RIP | Instructions ending at RIP |
|---|---|---|
| #0 | `er+0x582e79` | `mov rax,[rip+0x37e4d5e]` ; **`mov [rax+0x120], rbx`** |
| #1 | `er+0x5830b1` | `mov rax,[rip+0x37e4b2a]` ; **`mov qword [rax+0x120], 0`** ; then `add rsp,0x38 ; pop r15 ; pop rbx ; ret` |

**The singleton RVA is confirmed twice, independently.** Resolving each rip-relative displacement
against its own next-instruction address:

- `0x582e72 + 0x37e4d5e = 0x3d67bd0`
- `0x5830a6 + 0x37e4b2a = 0x3d67bd0`

⇒ precisely the `DAT_143d67bd0` named in §2. Two separate computations landing on the same address is
a confirmation, not a coincidence.

Hit #1's clear is immediately followed by the function epilogue, bounding the enclosing function at
roughly `0x582d50 .. 0x5830b9` — consistent with `FUN_140582d50`, the per-tick step (`CSEmkEventIns`
vtable slot 1). So the EMEVD kernel is **live, stepping, and laid out as the static pass described**.

### 10.4 Why direct polling of the slot reads zero

Three `mem_dump` reads of `CSEmkSystem+0x120` all returned `00 00 00 00 00 00 00 00`. Not a
contradiction: the slot is **set and cleared within the same step** (§10.3), and the RPC samples from
the Present thread, i.e. always between steps. **Poll this slot and you will always see zero** — use
the watchpoint, or the hook.

### 10.5 Probe cost and game state

The probe is self-limiting: **7 `[FWA]` log lines total** (1 arm + 2 hits × 3 lines). It did not
flood, and the game kept running normally afterwards.

The game exited ~10 s later. Recorded so a later reader does not misread it as a probe crash: the log
ends with a complete `[BENCH] ===== END REPORT =====` (the DLL's normal shutdown path), **no crash
file was produced**, and the only entries between the hits and the shutdown are a `WM_SETFOCUS` then a
`WM_KILLFOCUS` — an alt-tab followed by a clean exit. Consistent with the user closing the game; a
probe-induced fault would be immediate and would not emit the shutdown report.

### 10.7 ✅ The main-menu discriminator — ANSWERED, and in a stronger form

Run at the main menu on a fresh boot (`frame=1796`, `menucover=1`, `coords` → `err not in-world
(LocalPlayer null / loading)`), same `er_base = 0x7ff675600000`:

```
rpc mem_dump 0x7ff679367bd0 8        # = er_base + 0x3d67bd0, the CSEmkSystem slot
ok 0x7ff679367bd0: 00 00 00 00 00 00 00 00
```

| | main menu | in-world |
|---|---|---|
| `coords` | `err not in-world` | `area=60` (Limgrave) |
| `CSEmkSystem` @ `er+0x3d67bd0` | **`0x0`** | `0x7ff471503b60` |
| EMEVD step | impossible — would write `[null+0x120]` | fires 6 ms after arming |

**Better than the planned test.** The plan was "arm the watchpoint and confirm silence", but absence
of hits is weak evidence (many things cause silence). Instead the **singleton itself is NULL**, which
is a positive measurement: the EMEVD subsystem is not instantiated outside a world, so the step
cannot be running — and the `+0x120` target is not even an addressable location at the menu.

⇒ **The EMEVD kernel is WORLD-scoped, not process-scoped. The dispatcher is the real VM, not a
generic look-alike.** The prompt's discriminator is satisfied.

### 10.8 Bonus — where `CSEmkSystem` is constructed (3rd independent confirmation of the slot)

Watchpoint armed on the **singleton slot** itself (`er+0x3d67bd0`, write) at the menu, then a save was
loaded. Hit at `rip = er+0x66e397`, decoded from the byte window:

```asm
test rax, rax
jz   +0x0b
mov  rcx, rax
call er+0x585af0              ; inside the Emk range 0x560000-0x590000
mov  rbx, rax
mov  [rip+0x036f9839], rbx    ; -> er+0x3d67bd0  = the CSEmkSystem slot
mov  byte [rdi+0xb7c1], 1     ; a "ready" flag
```

So `CSEmkSystem` is **lazily constructed at world load** and the returned pointer stored into the
slot. `0x66e397 + 0x36f9839 = 0x3d67bd0` — a **third** independent confirmation of the slot, this
time from a code site unrelated to the two in §10.3.

**⚠ This probe crashed the game** — see `docs/memory/tooling/mfg-rpc-driver-hardening.md`
("`mem_fwa` — NEVER leave it armed across a world LOAD"). The hit was captured, the load continued
~1.3 s, then the process died hard: the log stops mid-activity with **no** `[BENCH] ===== END REPORT
=====` (contrast §10.5's clean exit) and **no crash triage file**. `mem_fwa` does not auto-disarm
after a hit, so the DR0 breakpoint stayed live on 96 threads through the whole load. Disarm before
any transition. (Ruled out on inspection: the FWA VEH does *not* swallow access violations — it
returns `EXCEPTION_CONTINUE_SEARCH` for anything that is not `EXCEPTION_SINGLE_STEP`,
`goblin_field_probe.cpp:109`.)

### 10.9 Still open

- **The condition-group byte at `cond+0x00`** — the one inferred field left in the chain. Not
  reachable by a point probe (it needs a live condition object held long enough to dump); it falls
  out of the G1 hook itself.

**Net:** G1's foundation is no longer static-only. The dispatcher address, the singleton (confirmed
three times from three distinct code sites), the per-step write, and the world-scoped lifetime are
all measured on a running 2.6.2.0 **vanilla** game.
