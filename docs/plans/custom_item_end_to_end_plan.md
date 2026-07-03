# Custom item end-to-end (Gap C) — plan

Status: **END-TO-END DONE (2026-07-03).** DEFINE(clone+fields+name) + GRANT + SIDECAR clean-save +
the `custom_items.toml` AUTHOR SURFACE all proven live. A hand-authored TOML file produces a named
custom item that grants into inventory, stays out of the vanilla `.sl2`, and re-applies every boot —
a custom item that DOESN'T ship a `regulation.bin`. Tests: `test_gapc_grant.py` 4/4 + clean 1/1,
`test_author_items.py` 1/1. Remaining polish only: finalize the reserved band from a param-scan
survey; the `decode_textid` read-back chain parity (menu-first); more categories as needed.

**Two blockers found while proving the grant (2026-07-03, `mfg run`/GameSession probes):**
- **Grantable goods-id CEILING = `0x7FFFFE` (8388606).** `give_item` no-ops for any goods id ≥
  `0x7FFFFF` (the goods id is a 23-bit field, `0x7FFFFF` reserved). Verified: 8388606 grants,
  8388607/8388608/9M/50M all no-op. **So the old reserved band `90000001` was NEVER grantable.** The
  grant itself is otherwise fine for a cloned row (0→1); the earlier "grant VERIFIED" only ever tried
  pre-existing low ids. Real ER goods ids sit far below the ceiling.
- **`fmg_set` slot matters — base `10` WORKS, DLC-tier `419` FREEZES.** The name lives across tiers
  `kGoods={419,319,10}`; **base slot 10** injects instantly (`ok 10:<id>=<text>`, game alive, reads
  back via `GetMessage`). Slot **419** hangs `patch_fmg_in_memory` on the PRESENT thread (RPC marshals
  there) → no frames → game dead; a group-span guard did NOT stop it, so 419 is structurally different
  (not just a big group range). Immediate path: inject names at slot 10. Open: (a) confirm the slot the
  game's item-name UI actually reads for a goods id (so the name renders on the item, not just our
  read-back), (b) why 419 hangs + an O(1) reject for hazardous slots. **Handed to a Windows/Ghidra sweep:
  `docs/re/windows_fmg_slot_re_prompt.md`.** The old fmg_set RPC comment said "419 GoodsName" — WRONG,
  corrected to base 10/11/19.

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
4. ✅ Author surface — **DONE 2026-07-03.** `custom_items.toml` (TOML chosen over JSON for
   hand-authoring — see [[../memory/process/authoring-format-decision]]) `[[goods]]/[[weapon]]/…`
   array-of-tables = `{id, clone, name, qty, fields{}}`. `goblin_custom_items.{hpp,cpp}` (toml++,
   header-only) applies each at BOOT after setup_messages: `param_clone_row` + `param_set_field_by_name`
   + `inject_fmg_entries`(base name slot) + `sidecar::register_author_item` (a DECLARATIVE registry,
   granted on world-enter + stripped pre-save, NEVER written to the `.mfg` — the toml is the source of
   truth, re-applied every boot since param_clone/FMG don't persist). Verified E2E (ERR/Proton):
   `test_author_items.py` 1/1 — toml → boot define+name+register → world-enter grant →
   `goods_count == qty`. Categories wired: goods(10)/weapon(11)/protector(12)/accessory(13); each
   enforces the `0x7FFFFE` grantable ceiling. Example: `custom_items.example.toml`.

## Showing a custom item ON THE MAP (Gap C ⋈ MapForGoblins markers)
A custom item is inventory-only; a map marker is a WORLD PLACEMENT. The map resolves a loot marker's
identity LIVE via `AssetEnvironmentGeometryParam[aegRow].pickUpItemLotParamId → ItemLotParam_map →
item` (`msbe_parser.cpp:298`, `map_entry_layer.cpp` `aeg_pickup_lot`/`resolve_loot_item_textid`).

- **Repoint mechanism PROVEN live (2026-07-03).** New dev RPC `loot_at <aegRow>` resolves exactly what
  the marker build would show. `param_setf AssetEnvironmentGeometryParam <aeg> pickUpItemLotParamId
  <lot>` then re-reading `loot_at` changed the item at asset 99036 Bloodrose→Erdleaf Flower→Poisonbloom.
  So repointing an existing treasure's lot changes the map marker's item, no MSB edit — regulation-free.
- **✅ Custom item ON the marker chain PROVEN (2026-07-03).** `ItemLotParam_map/_enemy` `lotItemId01`
  (@0x00), `lotItemCategory01` (@0x20), `lotItemBasePoint01` (@0x40) are now settable — offset-only
  FieldSpecs (`goblin_param_edit.cpp`; core-stable offsets already read raw in `goblin_loot_resolve.cpp`;
  AOB upgrade prompt `windows_itemlot_field_re_prompt.md`). Setting an EXISTING lot's `lotItemId01` to
  the custom goods id made `loot_at 99036` resolve `item_textid=508000000 name='Goblin Test Item'` — the
  custom item AND its custom name (injected at base slot 10) render on the marker chain. The name-timing
  worry is a NON-issue: the marker name lookup resolves live through to slot 10.
- **Two remaining, both = the `refresh_markers` follow-up (HANDOFF), not blockers:**
  1. **Existing lots only.** A NEWLY CLONED lot isn't found by `resolve_loot_item_textid` — the
     `LotReader` lot INDEX is snapshotted at init. Modify an EXISTING lot in place (works) until the
     LotReader index is rebuildable. (Enough for re-skinning existing loot to a custom item.)
  2. **Drawn marker vs live resolve.** `loot_at` reads live, but the DRAWN markers build once at boot
     (`prebuild_markers` `call_once`), no live rebuild in the deployed build. So a visual marker update
     needs the edit before marker-build OR a `refresh_markers` (reset the `call_once` + the LotReader
     index + rebuild buckets).
- **New locations (a NEW treasure/mob at NEW coords) still need MSB editing** — outside the runtime
  framework. Repointing only RE-SKINS existing placements.

## Acceptance (mod-agnostic)
On vanilla AND a non-ERR mod: the define half produces a coherent named custom item from that install's
own template/params; the grant (post-sidecar) puts it in inventory and the `.sl2` stays vanilla-legal
(loads DLL-less with the item merely absent).
