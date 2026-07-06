# RE findings — the ER IN-COMBAT state (gate the vmap like the native map)

Answers `combat_state_gate_re_prompt.md`. **Route 2 (Windows Ghidra, `D:\ghidra_proj2\ER`, build 2.6.2.0,
imagebase 0x140000000).** Route 1 (Linux-live diff-scan) still owns the final confirmation — see the checklist.

## TL;DR — the flag
ER's per-entity **battle state** is an AI-FSM enum on the ChrIns "AI think" module:
```c
// FUN_1402c31d0 (er+0x2c31d0) — registered to ESD/AI as "IsBattleState"
bool IsBattleState(ChrIns *c) {
    void *ai = *(void**)((char*)c + 0xC950);   // AI think module (NPCs only)
    if (!ai) return false;
    return *(int*)((char*)ai + 0x30C) == 6;     // FSM state 6 = BATTLE
}
```
`*(int*)(*(ChrIns+0xC950)+0x30C)` is the AI FSM state; observed enum values **1,3,4,5,6** (6 = battle/combat,
5 = alert, 1/3/4 = neutral/search variants — see the sibling getters).

**⚠ The PLAYER has no AI think module → `[player+0xC950]` is (almost certainly) NULL → `IsBattleState(player)`
is always false.** So MFG's `combat_active()` must NOT read the player; it mirrors ER, which computes
"in combat" as an **aggregate over enemies**: *is any nearby enemy in battle state (6)*.

## How ER computes it (the aggregate)
- **Counter `FUN_1405d8790` (er+0x5d8790):** grabs `WorldChrMan` (`DAT_143d65f88`, the repo's WCM static
  RVA 0x3d65f88), builds a ChrIns candidate list via `FUN_140507ca0(WorldChrMan, &list, param_1)`, then for
  each entry calls the per-entity getter and **counts** the matches into `*param_2`.
- **Per-entity multi-state getter `FUN_1405f0750(chr, selector)` (er+0x5f0750):** reads the SAME
  `[[chr+0xC950]+0x30C]` field through 5 sibling predicates and returns one by `selector`:
  | selector | predicate (fn) | `[+0x30C] ==` |
  |---|---|---|
  | 0 | "none of the below" (fully neutral) | — (true when state ∉ {1,3,4,5,6}) |
  | 2 | FUN_1402c32a0 | 5 (alert) |
  | **3** | **FUN_1402c31d0 = IsBattleState** | **6 (battle)** |
  | 4 | FUN_1402c4a90 | 3 |
  | 5 | FUN_1402c4a70 | 4 |
  | 6 | FUN_1402c47d0 | 1 |
  So **`FUN_1405d8790(list, {&count, selector=3})` = number of enemies in battle state** → `>0` = in combat.

## How it was pinned
`strx.java` string-xref hunt → `"IsBattleState"` @ er+0x2a04b90, referenced by the AI-func registration
`FUN_140089710` (er+0x89710): registers a `DLRF::DLConcreteMethodInvoker<CS::CSAiFunc,bool,…>` named
`"IsBattleState"` with impl **`FUN_140300780`** (er+0x300780) = tail-call `FUN_1402c31d0(*(ctx+8))` (ctx+8 =
the ChrIns). Sibling `"IsInsideBattleArea"` (reg `FUN_140089160`, impl `FUN_140300d80`) is the arena-boundary
check, not general combat. `.?AVNearEnemyFinder@CS@@` (RTTI, vtable er+0x2a252c0, ctors 0xea4760/0x3923a0/
0x391600) is ER's own nearby-enemy iterator — the same role the CSFeMan enemy-bar list already fills for MFG.

## Anchors / offsets
```
AI think module ptr   ChrIns + 0xC950      (null for the player — enemies only)
AI FSM state (int)    [ChrIns+0xC950] + 0x30C   (6 = BATTLE; 5 alert; 1/3/4 neutral/search)
IsBattleState impl    FUN_1402c31d0  er+0x2c31d0
   entry AOB          48 8B 89 50 C9 00 00 48 85 C9 0F 85 40 7F 07 00 32 C0 C3
                      (mov rcx,[rcx+0xC950]; test rcx,rcx; jne null; xor al,al; ret — 0xC950 is in the AOB)
per-entity getter     FUN_1405f0750  er+0x5f0750   (selector 3 = battle)
enemy-count aggregate  FUN_1405d8790  er+0x5d8790   (WorldChrMan list → count by selector)
AI-func registration  FUN_140089710 "IsBattleState" / FUN_140089160 "IsInsideBattleArea"
WorldChrMan static    er+0x3d65f88   (already in repo: WCM_FINDER + fixed RVA)
NearEnemyFinder       vtable er+0x2a252c0 ; ctors 0xea4760/0x3923a0/0x391600
```

## Recommended MFG implementation (reuse existing infra)
`goblin_enemy_names.cpp` ALREADY resolves WorldChrMan and iterates the **CSFeMan enemy-bar array** (nearby /
on-screen enemies) → `GetChrInsFromHandle` → ChrIns, reading fields SEH-guarded. Add `combat_active()` on that
exact path:
```c
// per enemy ChrIns `chr`, inside the existing SEH body:
void *ai = *(void**)((char*)chr + 0xC950);
if (ai && *(int*)((char*)ai + 0x30C) == 6) combat = true;   // any enemy in battle state
```
`combat_active()` = OR over the enemy-bar set. Then in `panel_virtual_map` / host tick:
`if (overlay_api::vmap_redirect() && goblin::combat_active()) overlay_api::set_vmap_redirect(false);`
(force-close the vmap the instant combat starts — mirrors ER's own map-availability). The same
`combat_active()` later gates the vmap grace-warp (refuse in combat). The CSFeman enemy-bar list is the
"nearby" set (≈ NearEnemyFinder), so no separate radius query is needed.

## Confidence + what Route 1 (Linux-live) must confirm
- **HIGH:** `IsBattleState(chr) = *(int*)(*(chr+0xC950)+0x30C)==6` — the string name proves the semantic; the
  AOB embeds the 0xC950 offset.
- **MEDIUM:** that the aggregate-of-enemy-battle-state is EXACTLY ER's map/warp gate. The map-open combat block
  itself was NOT traced statically — the create-callback (`FUN_1407fd4b0`) is dispatched from a menu-factory
  table (`er+0x2abb910`) reached indirectly, and the input→menu-command edge is VMProtected (see
  `windows_input_path_re.md`), so there is no clean static `if(in_combat) return` to read. But the
  enemy-battle-state aggregate is how ER derives "in combat" everywhere else and matches the "foes nearby" gate.
- **Live checklist:** (1) confirm `[player+0xC950]==0` (player-side dead-end). (2) aggro an enemy, watch its
  `[[chr+0xC950]+0x30C]` flip to **6** (and back to 5→1 as it de-aggros); verify timing matches ER's own
  map-block (open the native map with `vmap_on_map_key=0`: blocked exactly while some enemy reads 6). (3) with
  `combat_active()` wired, open the vmap → aggro → vmap must auto-close the instant combat starts, reopen after.
  Tools: `tools/boot_hold.py` (HOLD ER) + `mfg.py rpc mem_dump` on the enemy ChrIns; aggro live.

Cross-ref: `combat_state_gate_re_prompt.md` (the brief), `native_map_redirect_linux_re_plan.md` (the redirect +
create-callback `FUN_1407fd4b0`), `windows_input_path_re.md` (why the map-command edge is VMP/opaque).

## LIVE (2026-07-06) — the entityHpBars source is WRONG (lock-on only) + the HP-bar ChrIns has no AI module
Wired `combat_active()` first over the name feature's **entityHpBars[8]** list (reuse). Two live problems:
1. **`[[HP-bar ChrIns + 0xC950] + 0x30C]` reads NULL** even for a normal enemy (Godrick Soldier, chr valid — has
   modules at +0x190, vtable ok). So the ChrIns that `GetChrInsFromHandle` returns from an HP-bar handle is a
   DIFFERENT object than the one ER's getter `FUN_1405f0750`/`IsBattleState` expects — its `+0xC950` (AI think
   module) is unpopulated. The findings' offset is for the **WorldChrMan-list** ChrIns, not the HP-bar one.
2. **entityHpBars only populate on LOCK-ON** (user live): an enemy attacking you unlocked shows no HP bar →
   `combat_active()` misses it → the vmap stays open. Too narrow — ER's "in combat" is any nearby enemy in
   battle state regardless of lock.
⇒ **Shipped a practical stopgap** (`ce82c58c`): `combat_active()` = any enemy HP bar present (closes the vmap
in locked combat; the map key can't close it in combat since ER blocks the create-cb, so auto-close is the
only way). **★ PRECISE follow-up = iterate the WorldChrMan enemy list** (NOT the HP bars): either drive
`FUN_1405d8790(param_1, {&count, selector=3})` (er+0x5d8790, builds the list from WorldChrMan DAT_143d65f88 via
FUN_140507ca0 + counts battle-state), or RE the WorldChrMan enemy-ChrIns array offset and iterate it directly
checking `[[chr+0xC950]+0x30C]==6`. That chr HAS the AI module (unlike the HP-bar chr). Live-doable on this box
(WCM resolves via WCM_FINDER / er+0x3D65F88).
