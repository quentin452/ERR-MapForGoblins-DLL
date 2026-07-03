# Windows-Ghidra RE findings — the "owned item count" read (sidecar Phase-2 clean-save ORACLE) — FOUND

Answers `windows_goods_count_re_prompt.md`. The prompt asked for `GetOwnedItemCount(id)` (or the
handle-resolver + struct offsets) so the sidecar can assert an item is GONE from a clean vanilla save.
**Delivered option (3): the direct EquipInventoryData walk** — read-only, no game call, no thread
concern. It's the safest oracle (a memory-walk that WORKS, because we now walk the RIGHT struct with
the RIGHT handle indirection the blind 2-level DIFF probe missed).

Done statically in Ghidra (`D:\ghidra_proj2\ER`, `-noanalysis`) via `tools/ghidra/rtti_index.txt` +
`query.java` + a bespoke `find_goodscount.java`. Imagebase `0x140000000`; RVAs are `er_base`-relative
for this ERR build — verify the layout live on the deploy build (see "Verify" below).

## The chain — GameDataMan → the held inventory

```
GameDataMan (static, GAME_DATA_MAN sig)
  → [+0x08]  PlayerGameData
  → [+0x2B0] EquipGameData          (already resolved by goblin::inventory::equip_game_data())
  → [+0x158] EquipInventoryData     ← the player's CARRIED inventory (what saves + what give_item feeds)
```

`EquipGameData+0x158` is the carried inventory: `FUN_140247160` (the equip dispatcher, param_1 =
EquipGameData) drives it as `FUN_14024c560(param_1 + 0x158, …)`. It is the one the save serialize
(`FUN_140257f20`, player-data serialize) snapshots and the one `give_item`/AddItemFunc updates through
MapItemMan. (There is a second EquipInventoryData for the storage box; not needed for the grant oracle.)

## EquipInventoryData layout (the two-segment slot list)

Every inventory method (`FUN_14024be90`, `…cb70`, `…cca0`, `…c460`, `…c560`, `…d1e0`, `…bfe0`,
`…e770`, …) shares ONE iteration idiom — a split list of two contiguous segments:

| offset | type      | meaning                                                             |
|--------|-----------|--------------------------------------------------------------------|
| `+0x08`| struct    | id→index hashmap (`FUN_140713xxx`; fast by-id lookup — optional)   |
| `+0x1C`| `u32`     | **seg1_count** — split point (indices `[0,seg1_count)` = segment 1)|
| `+0x40`| `ptr`     | **seg2_base** — "normal items" node array                          |
| `+0x50`| `ptr`     | **seg1_base** — "key/special items" node array                     |
| `+0x80`| `i32`     | **last_index** — occupied span = `last_index + 1` (−1 ⇒ empty)     |

Node **stride = `0x18`** (24 bytes). Index→node:
```
base = (i < seg1_count) ? seg1_base : seg2_base
idx  = (i < seg1_count) ? i         : i - seg1_count
node = base + idx*0x18
```

## Inventory node (0x18 bytes) — the accessors that pin the fields

The tiny accessors (all confirmed by decompile) give the field offsets directly:

| accessor        | body                          | ⇒ node field                          |
|-----------------|-------------------------------|---------------------------------------|
| `FUN_140712810` | `return *(i32*)node != 0`     | `@+0x00` handle/active (0 ⇒ empty slot)|
| `FUN_140712790` | `*out = *(u32*)(node+4)`       | **`@+0x04` itemId** (category-encoded) |
| `FUN_1407127a0` | `return *(u32*)(node+8)`       | **`@+0x08` quantity**                  |

`FUN_1407127a0` is the sibling immediately after `FUN_140712790` (its AOB tail is literally
`… 8B 41 08 C3` = `mov eax,[rcx+8]; ret`). Cross-checked two independent ways:
- the **decrement** path `FUN_14024bfe0` reads current qty via `FUN_1407127a0(node)`, computes
  `max(0, qty+delta)`, writes back — so `@+0x08` is the live stackable count give_item(±) moves.
- the `FUN_1407124b0`→`FUN_14024e770`→`FUN_140712670` ref chain: `FUN_14024e770` returns the node ptr,
  `FUN_140712670` reads `*(u32*)(node+8)` — same offset.

So the node is `{ u32 handle@0, u32 itemId@4, u32 quantity@8, …12 more bytes… }`.

### Item id encoding (matches the grant path)
`itemId@+4` is the category-encoded id. Goods ⟺ `(itemId & 0xF0000000) == 0x40000000`, base goods id =
`itemId & 0x0FFFFFFF` (confirmed in `FUN_14024cb70`/`…cca0`/`…e680`, and consistent with the grant
encoding `0x40000000|goodsId`). So to count a goods item, match `itemId == (0x40000000 | goodsId)`.

## The oracle — direct walk (no game call)

```c
uint32_t goods_count(uint32_t item_id /* = 0x40000000|goodsId for goods */) {
    inv  = *(u64*)(*(u64*)(GameDataMan) + 0x08)      // PlayerGameData
         + 0x2B0                                     // EquipGameData
         + 0x158;                                    // EquipInventoryData (carried)
    seg1 = *(u32*)(inv + 0x1C);
    s1b  = *(u64*)(inv + 0x50);
    s2b  = *(u64*)(inv + 0x40);
    last = *(i32*)(inv + 0x80);
    total = 0;
    for (i = 0; i <= last; i++) {
        base = (i < seg1) ? s1b : s2b;
        idx  = (i < seg1) ? i   : i - seg1;
        node = base + idx*0x18;
        if (*(u32*)node == 0) continue;              // empty slot
        if (*(u32*)(node + 4) == item_id) total += *(u32*)(node + 8);
    }
    return total;                                    // 0 ⇒ item ABSENT (clean-save assertion)
}
```

All reads are RPM-guarded (nulls before the world loads), read-only, no thread/save-timing concern —
safe from the present/RPC thread we already use for `give_item`/`warp`.

### Why the earlier blind DIFF probe failed (resolved)
The 2026-07-03 `goods_diff` scanned every 2-level array off PlayerGameData/MapItemMan for a plain i32
moving by the grant delta and found none — because it looked for the id *inline next to the qty*, but
the layout is `{handle@0, id@4, qty@8}` inside a **two-segment split list** at `EquipGameData+0x158`
(three pointer hops from GameDataMan, and the base/index selection straddles two arrays). Not reachable
by the blind ≤2-level scan. With the exact chain + segment split above, the walk is deterministic.

## Callable alternatives (not needed, recorded for completeness)
- `FUN_14024c460(inv, &item_id)` → slot **index** for an id (linear scan, `−1`/`0xffffffff` if absent).
  `−1` alone is a sufficient "item gone" oracle; the walk above also gives the quantity.
- `FUN_14024c560(inv, &item_id)` → sibling finder (via the `+0x08` hashmap; used by the remove path).
Both take `inv = EquipInventoryData` and an `int* id`. The direct walk is preferred (read-only, no
convention/thread risk, no AOB — reuses the existing GAME_DATA_MAN sig + struct offsets).

## Verify (Linux/Proton, no Windows dep)
Struct offsets are far more patch-stable than RVAs, but confirm once on the deploy build:
1. The DLL already exposes `equip_dump <off> <len>` (hex-dump EquipGameData+off) over RPC. Grant a
   known item (`give_item 0x40003bec 7`), then `equip_dump 0x158 0x90` and eyeball `+0x1c/+0x40/+0x50/
   +0x80`; dump a node array to see `{handle,id,qty}` at `0x18` stride.
2. `goods_count(0x40003bec)` must read `7`; after `give_item 0x40003bec -7`, read `0`.
This is the count-read the cap-oracle E2E (`test_custom_item.py`) needs to close Variant A.
