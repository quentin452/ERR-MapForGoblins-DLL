# RE prompt (Windows/Ghidra) — which container + ChrIns carries the live "in-combat" AI state?

> **✅ RESOLVED (2026-07-06, Windows Ghidra) — see `combat_state_gate_re_findings.md` § "RESOLVED — 0xC950 is
> a CS::CSAiThink member".** The premise was wrong: `0xC950` is NOT an `EnemyIns`/`ChrIns` offset — it is a
> member of a separate **`CS::CSAiThink`** object (sizeof 0xF0D0, pooled). `IsBattleState`'s arg is a CSAiThink,
> so `[EnemyIns+0xC950]` is garbage by construction. Entity→CSAiThink is manager-mediated (no fixed offset);
> the `FUN_1405d8790` aggregate is entity-ID-keyed (not a nearby scan); no clean global bool exists. Drop the
> `[[EnemyIns+0xC950]+0x30C]` plan → keep the HP-bar stopgap, or LIVE pointer-scan for an EnemyIns→CSAiThink
> cache, or iterate the CSAiThink pool. Q1–Q4 answered in the findings. Kept below for history.

**For the Windows Ghidra agent** (`D:\ghidra_proj2\ER`, build 2.6.2.0, imagebase 0x140000000). Follow-up to
`combat_state_gate_re_findings.md`. A first Linux-live implementation walked the wrong container and reads
garbage — `combat_active()`'s `battle` count is **always 0** even in real combat. Need the exact structure.

## What is already CONFIRMED (Linux-live, ER 2.6.2.0 under Proton — runtime bytes match Ghidra)
1. **`IsBattleState` = `FUN_1402c31d0` (er+0x2c31d0)** disassembled live end-to-end:
   ```
   er+0x2c31d0:  mov rcx,[rcx+0xC950]      ; rcx arg = a ChrIns; +0xC950 = AI-think module ptr
                 test rcx,rcx ; jne er+0x33B120 ; xor al,al ; ret     ; null AI → false
   er+0x33B120:  cmp dword [rcx+0x30C], 6 ; sete al ; ret             ; ← battle state == 6
   (sibling right below: cmp [rcx+0x30C], 5 → alert)
   ```
   So the semantic `[[chr+0xC950]+0x30C]==6` (offset **0x30C**, value **6**) is 100% correct **for whatever
   ChrIns type ER passes here**. That is not in question.
2. **`FUN_140507ca0` (er+0x507ca0)** — the list-walker — disassembled live:
   ```
   cmp [rcx+0x1CC58], ebx      ; rcx = WorldChrMan ; WCM+0x1CC58 = a COUNT
   lea rdi, [rcx+0x1CC60]      ; WCM+0x1CC60 = array, stride 0x18, [entry+0]=ptr, [entry+8]=id
   loop: mov rcx,[rdi]; call FUN_140494B30(entry0, list, param_1); lea rdi,[rdi+0x18]
   ```
   `FUN_140494B30` (er+0x494B30) reads **`[block+0x48]`** and does an MSVC `std::_Tree` / keyed lookup
   (`[node+0x19]` isnil, `[node+0x20]` key, `[node+0x10]` child) — a **targeted lookup by `param_1[0]`**,
   NOT a flat enumeration.
3. WorldChrMan slot = `er+0x3D65F88` (re-deref every call — the singleton pointer AND LocalPlayer object both
   reallocate on world transitions). LocalPlayer = `WCM+0x1E508`. All confirmed live.

## What went WRONG (the open question)
The Linux impl instead walked a **flat array at `block+0x18`** (count/cap at `block+0x10`, stride 0x10),
whose entries are RTTI-confirmed `.?AVEnemyIns@CS@@`. Two fatal problems, both seen live:
- **`[EnemyIns+0xC950]` on those block+0x18 entries is NOT an AI module** — it reads floats / a
  `.?AVGXFlverTexture@GXFLV@@` pointer / garbage. So `[+0xC950]+0x30C` is noise; it never equals 6.
- **`block+0x18` is volatile**: same block, minutes apart, went from ~33 `EnemyIns` to **zero**. It behaves
  like a transient render/spawn pool, not the logical enemy roster.
- The **actively-fought enemy** (HP bar → `GetChrInsFromHandle`) is also `CS::EnemyIns` and **also has garbage
  at +0xC950**.

So: `CS::EnemyIns` objects reachable from `block+0x18` and from the HP-bar handles do **not** have their
AI-think module at +0xC950 — yet `IsBattleState` clearly reads +0xC950. The ChrIns ER actually battle-tests
is a **different object/class or a different container** than what we found.

## Questions to answer in Ghidra (in priority order)
1. **What class does `IsBattleState`/`FUN_1405f0750` get called on, and is +0xC950 that class's AI-think
   module?** Walk callers of `FUN_1402c31d0`. Confirm the field type at `ChrIns+0xC950` (expect a
   `CS::CSChrAiThink`/`CS::AI`-ish module). Is +0xC950 the offset for the **base `CS::ChrIns`**, or specifically
   for a subclass that is NOT the `CS::EnemyIns` we pulled from `block+0x18`? (i.e. does `CS::EnemyIns` place
   its AI-think module at a DIFFERENT offset, with 0xC950 being render/other data for that subclass?)
2. **What is the real enemy roster ER battle-tests?** Fully trace `FUN_1405d8790` (er+0x5d8790, the
   battle-state **count** aggregate): what container does it feed to `FUN_140507ca0`, and what is `param_1`
   (the key list — `NearEnemyFinder`? a handle array)? Give the concrete offsets so MFG can walk the SAME set.
3. **`block+0x18` vs `block+0x48`.** Name the `WorldBlockChr` (block descriptor) fields: what is the flat
   array at `+0x18` (cap `+0x10`), and what is the tree at `+0x48`? Which one holds the live AI-carrying
   ChrIns? For the tree, give node→ChrIns value layout so MFG can enumerate it directly.
4. **Is there a SIMPLER global "in combat" signal** ER's map-disable actually reads (battle-BGM state,
   `CSFD4`/`CSFeMan` combat flag, a `WorldChrMan` bool)? The per-enemy aggregate was only MEDIUM-confidence as
   the real map gate; a single global flag would be far more robust than re-deriving the roster.

## Deliverable
A short findings doc: the exact chain `WorldChrMan → (container) → each ChrIns-with-AI → +0x??? AI module →
+0x30C state`, with offsets valid for build 2.6.2.0, OR the address/offset of a global combat flag. That lets
MFG's `combat_active()` mirror ER's map-disable correctly (verify: aggro an enemy with the vmap open → it
auto-closes; disengage → reopens).

## Cross-refs
`combat_state_gate_re_findings.md` (the SOLVED-then-corrected history + all runtime addresses),
`combat_state_gate_re_prompt.md` (original brief), `native_map_redirect_linux_re_plan.md` (why the vmap must
close in combat — redirect #3), `entity_radar_foundation.md` (WorldChrMan enemy enumeration was abandoned once
before — same wall).
