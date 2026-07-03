# Windows-Ghidra RE prompt — the "owned item count" function (sidecar Phase-2 clean-save ORACLE)

## Why this is needed

The sidecar Phase-2 strip/reinject bracket is WIRED and LIVE (`hk_serialize` on `SERIALIZE_FN`
`0x67dc00`, committed). To close it we need a **clean-save regression test**: grant a custom item →
game-save (bracket strips) → reload with the `.mfg` `[items]` emptied → assert the item is GONE from
the vanilla save. That assertion needs an **automated "how many of item X does the player hold?"**
read. We do not have one, and a memory-walk can't produce one (see the blocker below).

## The blocker (proven on Linux, 2026-07-03 — do NOT redo this by hand)

We tried to read the held count by walking memory from `PlayerGameData`
(`GameDataMan(0x143d5df38) → +0x8 → +0x2B0 EquipGameData`) and from the `MapItemMan` singleton
(`INVENTORY_ACCESSOR` static, the pointer `give_item` writes through). A before/after DIFF probe
(`goods_diff`: snapshot every 2-level array off both roots → `give_item(id, Δ)` → re-scan for a u32
that moved by exactly Δ) was the oracle:

- Scanning for the category-encoded id (`0x40000000|goodsId`) only ever hits **static catalog/param
  tables** (e.g. `{handle=0xb00000|id, id=0x40000|id, maxStack, sortId, …}`) — their value fields do
  NOT track grants (Golden Rune showed `0x2710`=10000, Thin Beast Bones `0x7`; both static across
  grant/remove).
- The DIFF found **no plain i32** within 2 pointer levels of either root that changed by the grant
  delta (`+5`, `+321`, `-3` all → 0 distinct changed slots, game frozen on the present thread).

Conclusion: the held **quantity is not stored inline next to the id** in any array reachable in ≤2
levels. ELDEN RING uses **GaItemHandle indirection** — the held/inventory list stores
`{GaItemHandle, quantity}` and the item **id** lives in a separate `GaItem` object resolved from the
handle through a hashmap owned by `MapItemMan`/`GaItemImp`. Reading a count by id therefore requires
either (a) reimplementing that hashmap resolution (fragile, version-specific), or (b) **calling the
game's own count function** — which is why this is a Ghidra handoff (the same play that cracked
`SERIALIZE_FN`).

## The ask — find `GetOwnedItemCount(id)` (or equivalent) in Ghidra

Deliver a function that, given a category-encoded item id (`0x40000000|goodsId` for goods), returns
how many the player currently holds. Good anchors:

1. **`EquipInventoryData::GetItemQuantity` / `GetInventoryItemNum`.** The inventory-data object off
   `PlayerGameData` (near the `EquipGameData` at `+0x2B0`). Look for a method that takes an item id,
   walks the held entries, resolves each `GaItemHandle → GaItem`, compares the id, and returns the
   `quantity` field. The serialize sub `FUN_140257f20` (player-data serialize, already RE'd — see
   `windows_save_serialize_re_findings.md`) touches `PlayerGameData`'s inventory chain and is a good
   starting xref neighbourhood.
2. **The shop "you already own N" check.** Merchant/inventory UI code that greys out or annotates an
   owned count calls exactly this. Search for callers that format an owned-count string.
3. **`GaItemImp` handle→GaItem resolver.** If no clean by-id counter exists, deliver the handle
   resolver (`GaItemImp::GetGaItem(handle) -> GaItem*`) + the `GaItem` id/qty field offsets, and the
   `EquipInventoryData` held-list layout (entries ptr + count + entry stride + `{handle,qty}`
   offsets). Then the DLL can walk it directly.

## Deliver

- RVA + **unique AOB** (`.text`, App 2.6.x → re-verify `[SIG]` on the ERR deploy build).
- Call convention (which reg is `this`/inventory-data, which is the id, return in `eax`?).
- Whether it's synchronous + safe to call from the present/RPC thread (we call `give_item`/`warp`
  that way already).
- If option (3): the exact struct offsets instead of a callable.

## Then (Linux, no Windows dep)

1. Add `SIG` entry + `goblin::inventory::goods_count(id)` = one guarded call (like `give_item`).
   RPC `goods_count <id>`.
2. Script `tools/rpc_tests/test_custom_item.py`: define+grant a reserved-id custom item → warp-save →
   empty the `.mfg` `[items]` → reload → assert `goods_count == 0` (clean vanilla save) while the
   sidecar re-grants it live. This is the Phase-2 clean-save regression that closes Variant A.

The `goods_diff` probe used to prove the blocker is NOT committed (reverted with the memory-walk
experiment); the recipe above is enough to rebuild it if the Ghidra route stalls.
