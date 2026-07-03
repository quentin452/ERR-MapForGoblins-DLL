# Custom item end-to-end (Gap C) — plan

Status: **DEFINE(stats) + GRANT + SIDECAR clean-save all PROVEN end-to-end (2026-07-03).** A CLONED
custom goods row can be granted into inventory and kept out of the vanilla `.sl2`
(`tools/rpc_tests/test_gapc_grant.py` 4/4 + boot-2 clean 1/1). Remaining: the NAME (Gap D) live path
freezes, the reserved band needs revising to the grantable range, and the author surface + boot-time
re-definition. The payoff of the runtime-modding framework: a custom item that DOESN'T ship a
`regulation.bin`.

**Two blockers found while proving the grant (2026-07-03, `mfg run`/GameSession probes):**
- **Grantable goods-id CEILING = `0x7FFFFE` (8388606).** `give_item` no-ops for any goods id ≥
  `0x7FFFFF` (the goods id is a 23-bit field, `0x7FFFFF` reserved). Verified: 8388606 grants,
  8388607/8388608/9M/50M all no-op. **So the old reserved band `90000001` was NEVER grantable.** The
  grant itself is otherwise fine for a cloned row (0→1); the earlier "grant VERIFIED" only ever tried
  pre-existing low ids. Real ER goods ids sit far below the ceiling.
- **`fmg_set 419` (GoodsName) FREEZES the present thread** — a live name injection hangs the game (no
  frames; `alive→False`). The name is cosmetic; the grant works without it. Gap D live path needs
  fixing (or do the name inject at boot, not via the live RPC) before the name ships.

Depends on the three shipped primitives (`goblin_param_edit` + `goblin_messages`), the frozen policy
[[../memory/process/reserved-id-and-load-contract]] (Gap H), and — for a clean grant —
`shadow_sidecar_save_plan.md`. Battle order in `docs/runtime_live_capabilities_audit.md`.

## A custom item = two halves

### Half 1 — DEFINE (save-safe) — ✅ DONE
Compose the three primitives; nothing persists (params + FMG reload from regulation/msgbnd each boot):
1. `param_clone_row(param, template_id, new_id)` — a new row cloned from a template (Gap B).
2. `param_set_field_by_name(param, new_id, field, value)` — customize its stats (Gap A).
3. `inject_fmg_entries(name_slot, {{new_id, L"…"}})` — its name (Gap D); GoodsName base slot = 10,
   WeaponName = 11, keyed by the item id.

**VERIFIED (ERR/Proton, via RPC): a custom goods item id `90000001`** cloned from goods 100,
`sortGroupId=101`, named "Ashen Custom Flask" — all read back correct, game stays alive. Mod-agnostic
(reads the active install's template + params), collision-safe (Gap H), zero save risk. This is the
whole item EXCEPT it isn't in the player's inventory yet.

### Half 2 — GRANT — PRIMITIVE BUILT + VERIFIED 2026-07-03 (still gated on the sidecar for a clean save)
`goblin::inventory::give_item(item_id, qty)` (`goblin_inventory.{hpp,cpp}`) calls the game's AddItemFunc:
`AddItemFunc(rcx=inv, rdx=&entry{qty:i32@0,id:u32@4}, r8=scratch buffer, r9=0)`. `inv` from the
`INVENTORY_ACCESSOR` static AOB (Hexinton CT; `[SIG] PASS`). Goods id = `0x40000000|goodsId`. Grant =
`qty>0`, remove = `qty<0` (ER has no separate RemoveItem — verified in-game via the item-cap dialog
oracle). RPC `give_item <id> <qty>`. So the GRANT mechanism is DONE; what remains is wiring it to the
custom item (define → grant) + doing it save-clean via the sidecar strip/reinject
(`shadow_sidecar_save_plan.md` Phase 2). Original blocker notes below (now resolved):

### Half 2 — GRANT (persisted, risky) — GATED, not started
Put the defined item into the player's inventory so they have it. This is the only persisted/risky
part. Requirements + blockers:
- **RE the inventory accessor — CAPTURE BOOTSTRAP DONE (2026-07-03), in-game capture pending.**
  `AddItemFunc` is AOB-resolved and hooked READ-ONLY as an observer (`goblin_debug_events.cpp:344`,
  `sig::ADD_ITEM_FUNC`). Convention: `AddItemFn(void* inv, entry* {qty:u32@+0, item_id:u32@+4}, base,
  count, pad)`. To CALL it we need the `inv` pointer. The observer now CAPTURES the live `inv` on any
  game grant → `goblin::debug_events::last_inventory_accessor()` (MapItemMan is a session singleton →
  reusable), + logs `[INVACCESS]` correlating it against LocalPlayer/WCM to derive a static path. RPC
  `inv_probe`. NEXT: drive a loaded ER, pick up an item, read the pointer/offset. Detail:
  `shadow_sidecar_save_plan.md` RE targets.
- **Item-id encoding — CONFIRMED for goods (2026-07-03).** A captured grant read
  `entry+0=0x00000001 entry+4=0x40003bec` → the id is `entry+4 = 0x40000000 | goodsId` (goods base
  `0x40000000`; qty in `entry+0`). So a custom goods id `G` is granted as `0x40000000 | G`. Weapon/armor/
  accessory bases (`0x0/0x10000000/0x20000000`) follow the standard ER category encoding — confirm each
  against a pickup before granting that category.
- **Save risk (why it waits for the sidecar).** A granted custom id writes into the `.sl2` inventory →
  per Gap H the DLL-at-load contract is HARD (orphan if the DLL is later removed). The clean path is
  the sidecar strip-and-reinject (`shadow_sidecar_save_plan.md`): keep the custom id OUT of the `.sl2`,
  re-grant on load. So: **do the grant AFTER the sidecar**, OR only as a clearly-labeled throwaway
  test on a throwaway save with explicit user consent — never silently onto the user's real save.
- **Needs a loaded save** (grant into a live inventory + open inventory to verify the named item).

## Reserved band (Gap H) — finalize here
The define test used `90000001` ad hoc — **which is NOT grantable** (> the `0x7FFFFE` ceiling above).
The band MUST live in `1 .. 0x7FFFFE`. `test_gapc_grant.py` uses `8000000` (0x7A1200), which grants
cleanly and is well clear of real ER goods ids. Before any grant ships, finalize the reserved
custom-goods band per [[../memory/process/reserved-id-and-load-contract]] from a live param-scan
survey of the target regulation(s) — pick a sub-range of `8000000..8388606` (or another gap below the
ceiling that the survey shows free), wire the ini base + the collision-check (already in
`param_add_rows`).

## Order of operations
1. ✅ Define half — stats (clone + fields) done + verified.
2. ✅ Sidecar strip/reinject clean-save (Variant A closed 2026-07-03) — the grant keeps the `.sl2` clean.
3. ✅ Grant half — a cloned custom goods row grants into a loaded inventory + is stripped from the save
   (`test_gapc_grant.py` 4/4 + clean 1/1). Uses id `8000000` (≤ the `0x7FFFFE` grantable ceiling).
   REMAINING in this step: (a) fix the NAME path (`fmg_set` GoodsName freezes — do it at boot or fix the
   live inject); (b) finalize the reserved band from a param-scan survey.
4. Author surface (NEXT): a `custom_items.json` (per [[../memory/process/authoring-format-decision]] —
   JSON for rich records) = `{clone, id, name, fields{}}`, applied at BOOT (define: clone+fields+name)
   + granted/registered via the sidecar path so it re-applies every load (param_clone does NOT persist;
   the sidecar re-grant does). This is the real end-user deliverable and the last integration.

## Acceptance (mod-agnostic)
On vanilla AND a non-ERR mod: the define half produces a coherent named custom item from that install's
own template/params; the grant (post-sidecar) puts it in inventory and the `.sl2` stays vanilla-legal
(loads DLL-less with the item merely absent).
