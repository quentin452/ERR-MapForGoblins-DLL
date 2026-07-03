#pragma once
#include <cstdint>

// Inventory grant / remove primitive (Gap C GRANT + sidecar Phase-2 strip-and-reinject).
//
// Calls the game's own AddItemFunc — the routine ER uses for every pickup/reward. Convention
// + AOBs from the Hexinton all-in-one CT (ItemGib, "Zodiacsl125"), reconfirmed live by the
// AddItemFunc observer (goblin_debug_events): AddItemFunc(rcx=inv, rdx=&entry{qty:i32@0,
// id:u32@4}, r8=scratch buffer, r9=0). `inv` = the MapItemMan singleton, resolved from the
// INVENTORY_ACCESSOR static AOB (no captured pointer needed).
//
// REMOVAL: ELDEN RING has no separate RemoveItem function (the comprehensive Hexinton CT
// exposes none) — the game removes via AddItemFunc with a NEGATIVE quantity. So give_item
// with qty<0 is the strip half of the sidecar; qty>0 is the grant/reinject half.
//
// Item ids are category-encoded: goods = 0x40000000 | goodsId (confirmed live), weapon =
// id, armor = 0x10000000|id, accessory = 0x20000000|id, ash-of-war/gem = 0x80000000|id.
namespace goblin::inventory
{
    // Resolve AddItemFunc + the MapItemMan static slot (idempotent; logs [INVGRANT]). Called
    // at init after params are ready. Safe no-op on any AOB miss (give_item then returns false).
    void initialize();

    // The live inventory accessor (MapItemMan) from the static slot; falls back to the
    // AddItemFunc-observer-captured pointer if the AOB didn't resolve. nullptr if neither.
    void *accessor();

    // Grant (qty>0) or REMOVE (qty<0) `qty` of the category-encoded `item_id`. Calls the game's
    // AddItemFunc on the CURRENT thread (caller must be a safe point — present thread for the
    // sidecar/RPC path). SEH-guarded. Returns false if unresolved or the call faulted.
    bool give_item(uint32_t item_id, int32_t qty);

    // Live EquipGameData pointer = GameDataMan(static) → +0x8 PlayerGameData → +0x2B0 EquipGameData
    // — the inventory the SAVE serialize reads (sidecar Phase-2 strip-bracket RE target). nullptr
    // before the world loads / on fault. SEH-guarded chain walk.
    void *equip_game_data();
}
