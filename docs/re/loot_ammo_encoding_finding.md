# Finding — the 81 loot "drifts" are an ammo-ENCODING mismatch, NOT a broken baker

Code/data investigation (no game-memory RE) of the `[LOOTID]` probe result: 4316 lot-backed
markers, 4235 same, **81 "drifted" — every one baked == live + exactly 100000000** (e.g. baked
150010000 → live 50010000). Question: is the baker broken, or is it an encoding bug?

## Verdict: the BAKER is correct. `encode_live_item` follows a DIFFERENT (also-valid) convention.

The codebase has **two coexisting ammo encodings**, both in `goblin_messages.cpp::setup_messages`,
and the two sides of the probe happen to follow different ones:

### Convention A — per-marker name copy (DEFAULT path, liveLootLabels OFF — ERR default)
`goblin_messages.cpp` lines 703-711:
```cpp
for (int32_t tid : weapon_ids_needed) {          // weapon_ids_needed = marker textIds in [100M,200M)
    if (tid >= 100000000) weapon_offset_ids.insert(tid);   // ALWAYS true here → ammo_ids stays EMPTY
    else                  ammo_ids.insert(tid);
}
copy_fmg_layered(weapon_slots, ammo_ids,        0,         "WeaponName(ammo)");   // DEAD (empty)
copy_fmg_layered(weapon_slots, weapon_offset_ids, 100000000, "WeaponName(weapon)");
```
Markers reach `weapon_ids_needed` only via `tid >= 100000000` (line 472), so **every** cat-2 marker
textId is ≥100M → the `ammo_ids`/offset-0 branch is **dead**. All cat-2 (ammo AND weapons) are
copied with `offset_base = 100M`: real FMG id = `tid − 100M`, looked up in `WeaponName` (which
contains ammo names too), and the PlaceName entry is written at the marker's textId. So a baked
ammo marker `150010000` → WeaponName[50010000] (the ammo name) → PlaceName[150010000]. **Resolves
correctly.** → the generator's "+100M for all cat-2" (CATEGORY_OFFSETS) is RIGHT for the path that
actually ships.

### Convention B — liveLootLabels full copy (OFF by default)
Line 731: `copy_fmg_all_layered(weapon_slots, 100000000, "WeaponName", ammo_as_is=true)`, with
line 655: `enc = (ammo_as_is && id >= 50000000) ? id : id + offset_base`. Here ammo (id≥50M) is
copied at its **raw** id (PlaceName[50010000]), weapons at +100M.

### `encode_live_item` (goblin_inject.cpp) follows Convention B
`case 2: return (item_id >= 50000000) ? item_id : item_id + 100000000;` → ammo raw. That matches
Convention B, NOT the default Convention A the baker uses. `encode_live_item` had **no callers**
before the dormant `resolve_loot_item_textid` probe, so this divergence never shipped.

## Conclusion
- **Not a baker bug, not real loot drift.** The 81 are the probe comparing the baked Convention-A
  ammo encoding (+100M) against `encode_live_item`'s Convention-B (raw). Both are internally valid;
  they just disagree on ammo.
- **No shipping impact:** markers display their baked `textId1`, resolved by Convention A (default
  path) → correct names. The mismatch only exists inside the dormant probe.
- **Safe to push.** The boss + loot work is sound; this is a documented dormant inconsistency.
- **If phase 2 (live loot identity) is ever pursued:** align `encode_live_item`'s cat-2 case to
  Convention A (always `+100M`, matching the baker + the default name path) — OR require
  liveLootLabels ON so Convention B's raw-id ammo entries exist. Until then, leave it dormant.
