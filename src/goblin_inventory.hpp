#pragma once
#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API (read_run_stats is called from the render module)

#include <cstdint>
#include <string>
#include <vector>

// Inventory grant / remove primitive (Gap C GRANT + sidecar Phase-2 strip-and-reinject).
//
// Calls the game's own AddItemFunc — the routine ER uses for every pickup/reward. Convention
// + AOBs from the Hexinton all-in-one CT (ItemGib, "Zodiacsl125"), reconfirmed live by the
// AddItemFunc observer (goblin_debug_events): AddItemFunc(rcx=inv, rdx=&entry{qty:i32@0,
// id:u32@4}, r8=scratch buffer, r9=0). `inv` = the MapItemMan singleton, resolved from the
// INVENTORY_ACCESSOR static AOB (no captured pointer needed).
//
// REMOVAL: AddItemFunc is ADD-ONLY — give_item(qty<0) is a NO-OP, live-verified 2026-07-03
// (the CT's "negative qty removes" claim is wrong for this build; the count never moves). The
// real remove is a direct decrement of the EquipInventoryData node's qty field — exactly what
// the game's own decrement path FUN_14024bfe0 does (reads qty via FUN_1407127a0, writes back
// max(0,qty+delta)). The sidecar strip snapshots + zeroes the matching node(s) via strip_goods()
// and restores them post-serialize via restore_goods(). See windows_goods_count_re_findings.md.
//
// Item ids are category-encoded: goods = 0x40000000 | goodsId (confirmed live), weapon =
// id, armor = 0x10000000|id, accessory = 0x20000000|id, ash-of-war/gem = 0x80000000|id.
namespace goblin::inventory
{
    // One stripped inventory node: its absolute address + the 0x18 original bytes, so the
    // save-bracket can zero the slot before serialize and restore it byte-exact afterwards.
    struct StripEntry
    {
        uintptr_t node;       // absolute node address in EquipInventoryData
        uint8_t   bytes[0x18];
    };
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

    // Run-tracker counters, off the SAME GameDataMan static slot the equip chain walks:
    // `deaths` = GameDataMan+0x94 (u32), `igt_ms` = GameDataMan+0xA0 (u32, MILLISECONDS).
    // Both are save-serialized, so they survive a reload and match the game's own load screen.
    // Mod-agnostic by construction — engine save data, not param/FMG content, so it reads the
    // same on vanilla, ERR or any other mod. Returns false (outputs untouched) before the world
    // loads / on fault; callers should keep their last good values rather than showing 0.
    GOBLIN_RENDER_API bool read_run_stats(uint32_t &deaths, uint32_t &igt_ms);

    // Native persistent bloodstain (dropped runes on death). 2.6.2.0 layout, Ghidra-verified
    // (docs/re/windows_bloodstain_read_drift_re_findings.md): `exists` = the flag byte
    // GameDataMan+0x40 — the ENGINE's own icon gate, set on ANY death incl. a 0-rune one —
    // AND souls >= 0 (the record is -1-cleared when none). blk = [GameDataMan+0x48]:
    // X/Y/Z @ +0/+4/+8 (block-local physics frame; the engine REBASES the stored value when
    // the streaming origin moves, so it is not stable across warps), runes @ +0x34 (0 is a
    // VALID existing stain — do NOT gate on souls>0), mapId @ +0x38. Returns false only when
    // the chain is unresolvable (menu/load).
    bool read_bloodstain(bool &exists, float &x, float &y, float &z, uint32_t &mapid, int32_t &souls);
    // One-line dump of the resolved bloodstain chain (AOB match/slot er-RVAs, gdm, blk, flag,
    // raw record hex) — the `bloodstain_probe dbg` RPC body. RPM-guarded, safe anytime.
    std::string bloodstain_debug();

    // How many of the category-encoded `item_id` the player currently HOLDS (carried inventory).
    // Read-only RPM walk of EquipInventoryData (EquipGameData+0x158) — the two-segment slot list,
    // node {handle@0, id@4, qty@8} @ 0x18 stride (docs/re/windows_goods_count_re_findings.md). No
    // game call, no thread/save-timing concern. Returns 0 when absent (the sidecar clean-save
    // oracle) OR when the chain isn't resolvable yet (not in-world) — callers that need to tell
    // "absent" from "not-ready" should gate on equip_game_data() != nullptr first.
    uint32_t goods_count(uint32_t item_id);

    // Phase-2 clean-save strip: find every carried-inventory node whose id matches one of `ids`,
    // snapshot its 0x18 bytes, then mark the slot empty (zero handle@0 + qty@8) so the save
    // serialize writes it as absent. Returns the snapshots — pass them to restore_goods() the
    // instant the serialize returns. In-process WriteProcessMemory (guarded, no fault on a bad
    // ptr); synchronous, meant to run on the save thread inside the serialize bracket. A no-op
    // (empty result) when not in-world or nothing matches.
    std::vector<StripEntry> strip_goods(const std::vector<uint32_t> &ids);

    // Restore nodes zeroed by strip_goods() (writes the saved 0x18 bytes back). Idempotent.
    void restore_goods(const std::vector<StripEntry> &saved);
}
