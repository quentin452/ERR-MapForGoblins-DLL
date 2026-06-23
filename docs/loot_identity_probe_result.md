# Loot-identity drift probe — result (2026-06-23)

**Verdict: ERR does NOT randomize lot-backed loot. The baked item identity is correct. Phase 2
(wiring the live item id into the marker label/icon) is NOT worth doing.**

## What was measured

`probe_loot_identity()` (map_entry_layer.cpp, gated `diag_loot_flags`) compares, for every
lot-backed marker, the BAKED `textId1` against the LIVE `ItemLotParam` slot-1 item re-encoded by
`goblin::resolve_loot_item_textid()` (reads `lotItemId01 @+0x00`, `lotItemCategory01 @+0x20` —
the same live row `resolve_loot_flag` reads).

In-game result:
```
[LOOTID] lot-backed identity baked-vs-live: lot_backed=4316 same=4235 drifted=81 miss=0
```
- **98% identical** (4235/4316). ERR keeps vanilla loot at these lots — it is not randomizing them.
- The 81 "drifts" are **NOT real item swaps**: every one is baked == live **+ exactly 100000000**
  (e.g. baked 150010000 → live 50010000, baked 164540000 → live 64540000). Uniform +100M = an
  **encoding mismatch**, not 81 random swaps.

## The +100M mismatch (a dormant encode_live_item quirk, no shipping impact)

The bake's generator (`generate_loot_massedit.py` `CATEGORY_OFFSETS`) adds **+100M to ALL category-2
items** (weapon/ammo). But `encode_live_item()` (goblin_inject.cpp) special-cases cat 2:
`(item_id >= 50000000) ? item_id : item_id + 100000000` — so **ammo** (id ≥ 50M, cat 2) gets NO
offset live while the bake gave it +100M. The 81 are those ammo-in-lot rows.

`encode_live_item` had **no callers** before this probe, so the quirk never shipped. Which side is
correct depends on where `liveLootLabels` copies the ammo FMG (raw id vs +100M) — unresolved, and
moot unless phase 2 is pursued.

## Important: ERR ADDS items (additive mod, not a swap-randomizer)

ERR's nature is **additive** — it ADDS new items / loot, rather than shuffling what's in existing
vanilla lots. The 98%-same result fits this: existing lots keep their (vanilla) item, so per-lot
**identity does not drift**. The relevant drift for an additive mod is therefore **COVERAGE**, not
identity: items ERR adds in a newer version than the one we extracted `items_database.json` from
would be **missing markers** (a lot/item present live but absent from our bake), NOT a wrong name
on an existing marker.

→ The future-proof check that matters is a **coverage diff** (live `ItemLotParam` lots/items that
have no baked marker), not the identity probe above. The identity probe still serves to confirm
existing-lot identity stays stable across versions; a coverage probe would catch ERR's additions.
Our bake already includes ERR's added items as of the extracted regulation — the gap only opens
when ERR ships new content and we haven't re-extracted.

## Decision

- **No phase 2.** 98% no-change; the 1.9% that change would only activate the `encode_live_item`
  ammo quirk and risk regressing ammo-marker names. Baked identity stays the source.
- **`resolve_loot_item_textid()` + `probe_loot_identity()` kept DORMANT** (diag-gated, zero cost
  off) as the re-check tool: if a future ERR version randomizes loot, flip `diag_loot_flags` and
  re-read this summary. If `drifted` is then large and NOT a uniform +offset → real randomization →
  revisit phase 2 (and first reconcile `encode_live_item`'s ammo case with the generator).
- The big takeaway mirrors the boss probe: **the bake was not the problem.** goblin_map_data.cpp's
  loot identity is sound for this ERR build.
