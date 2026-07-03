# Custom item end-to-end (Gap C) — plan

Status: **DEFINE half DONE + IN-GAME VERIFIED (2026-07-03). GRANT half GATED** (needs the
inventory-accessor RE + should wait for the sidecar save so the `.sl2` stays clean). The payoff of the
runtime-modding framework: a custom item that DOESN'T ship a `regulation.bin`.

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
- **Item-id encoding.** The grant's `item_id` is category-encoded (goods vs weapon vs gem live in the
  id's high bits / a base offset) — confirm the goods encoding against a known pickup via the existing
  observer before granting a custom id.
- **Save risk (why it waits for the sidecar).** A granted custom id writes into the `.sl2` inventory →
  per Gap H the DLL-at-load contract is HARD (orphan if the DLL is later removed). The clean path is
  the sidecar strip-and-reinject (`shadow_sidecar_save_plan.md`): keep the custom id OUT of the `.sl2`,
  re-grant on load. So: **do the grant AFTER the sidecar**, OR only as a clearly-labeled throwaway
  test on a throwaway save with explicit user consent — never silently onto the user's real save.
- **Needs a loaded save** (grant into a live inventory + open inventory to verify the named item).

## Reserved band (Gap H) — finalize here
The define test used `90000001` ad hoc. Before any grant ships, finalize the reserved custom-goods
band per [[../memory/process/reserved-id-and-load-contract]] from a live param-scan survey of the
target regulation(s), wire the ini base + the collision-check (already in `param_add_rows`).

## Order of operations
1. ✅ Define half (done).
2. Sidecar save (`shadow_sidecar_save_plan.md`) — so the grant can keep the `.sl2` clean.
3. Grant half: RE `inv` accessor + goods-id encoding → call `AddItemFunc` → verify named item in a
   loaded save's inventory. Reserved band finalized.
4. Author surface: a `custom_items.json` (per [[../memory/process/authoring-format-decision]] — JSON
   for rich records) = `{clone, id, name, fields{}}`, applied at boot (define) + granted via the
   sidecar path.

## Acceptance (mod-agnostic)
On vanilla AND a non-ERR mod: the define half produces a coherent named custom item from that install's
own template/params; the grant (post-sidecar) puts it in inventory and the `.sl2` stays vanilla-legal
(loads DLL-less with the item merely absent).
