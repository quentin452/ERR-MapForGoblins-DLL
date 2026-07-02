# Merchant / shop item search — plan

Status: **Slice 1 DONE + IN-GAME VERIFIED (2026-07-02, `feat/merchant-search` `0acbf8f`). Slice 2
DEFERRED (user) — merchant naming needs ESD/EMEVD RE the live ERR shop-id data proved disproportionate
(see Slice 2). Slice 3 open.** Verified on ERR/Proton: `[MERCHANTSEARCH] 5485 items indexed` at boot; F1 search "telescope"
(shop-only, Kalé — no world marker) lists **"Telescope · buyable (unlock required)"** under a new
"Sold by merchants" heading, with the FR translations. Names resolve even at the title screen.

## Goal (user, 2026-07-02)

The F1 item searcher can NOT find items **sold by merchants**, nor items in a dynamic merchant GUI
(e.g. spirit ashes unlocked at "Twin Maiden Husks" after turning in a bell bearing). Make merchant
stock searchable. Map pins for merchants are a nice-to-have, NOT the ask — the ask is SEARCH.

## Why it's missing (architecture)

- The F1 search (`src/overlay_panel/panel_search.cpp`, `draw_item_search`) iterates **markers only**
  (`overlay_layers() → L->markers()`), token-matching `Marker.name_id`. To appear, an item must be a
  `Marker` with `name_id` + `worldX/Z` + `category`.
- Items that are **only buyable** (spirit ashes at Twin Maidens, bell-bearing-unlocked mats) have **no
  world placement** → no marker → invisible to search. `docs/lot_reachability.md` correctly classifies
  `shop-*` gives as position-less (not markers).
- Shop data IS read at runtime today, but only to DELETE markers: `map_entry_layer.cpp:1336`
  `build_shop_infinite_keys()` reads `ShopLineupParam` (`RawShopRow`, row 0x34) to drop "merchant
  phantom" markers (`drop_merchant_phantoms`). It reads only equipId/sellQuantity/equipType — not the
  event-flag gate, not the row id.

## Data on hand (proven)

- **Runtime ShopLineupParam read** (mod-agnostic): `from::params::get_param<RawShopRow>(L"ShopLineupParam")`.
- **Row layout** (`tools/paramdefs/ShopLineupParam.xml`, row 0x34): `equipId@+0x00 s32`,
  `value@+0x04`, `mtrlId@+0x08`, `eventFlag_forStock@+0x0C u32` (**bell-bearing unlock flag**),
  `eventFlag_forRelease@+0x10 u32` (sold-out flag), `sellQuantity@+0x14 s16` (-1 = infinite),
  `equipType@+0x17 u8` (0 weapon · 1 protector · 2 accessory · 3 goods · 4 gem/aow), `nameMsgId@+0x28`.
  Merchant identity is encoded in the **row id** (ER convention `merchantId*100 + slot`) — nothing
  decodes it yet.
- **Name/icon for free from name_id:** `encode_live_item(item_id, cat)` (`goblin_loot_resolve.cpp:84`),
  cat = ItemLotParam category (1 goods→+500M, 2 weapon→+100M, 3 protector→+200M, 4 accessory→+300M,
  5 gem→+400M). Map ShopLineup equipType→cat: `0→2, 1→3, 2→4, 3→1, 4→5`. The search's existing
  `lookup_text_utf8(name_id)` + `lookup_name_en_disk_utf8(name_id)` resolve the encoded id to the live
  game-language name + English alias, and the tooltip/icon path already understands the encoding.
- **Merchant positions** (for later pins) exist via `entity_world_pos(entity_id,…)` from the MSB
  entity cache, and Twin Maiden Husks is ALREADY a searchable `WorldQuestNPC` pin (`quest_npc_layer.cpp`,
  nameId 160000). MISSING: the shop-row → merchant-NPC-entityId join (lives in EMEVD/ESD `ShopOpen`
  events, unextracted) — that's the RE cost that gates real map pins.

## Slices

### Slice 1 — searchable shop-item index (info-only rows) — DONE + VERIFIED (0acbf8f)

Make every merchant-sold item findable in the F1 search, as info rows (no map locate yet).

1. **Host build** (in `map_entry_layer.cpp`, alongside `build_shop_infinite_keys`): a
   `build_merchant_search_index()` that iterates ShopLineupParam once and returns a deduped
   `vector<MerchantItem>{ int32_t name_id; bool infinite; bool gated; }`:
   - encode `name_id = encode_live_item(equipId, equipType→cat)` (skip equipId<=0);
   - `infinite = (sellQuantity == -1)`; `gated = (eventFlag_forStock != 0)` (bell-bearing / progression
     unlock);
   - dedup by name_id (OR the flags — an item sold at several shops stays one row).
   Store in a host static; expose `GOBLIN_RENDER_API std::vector<MerchantItem> merchant_search_items()`
   (mirror the `harvested_ids` accessor pattern). Rebuild on each bucket build (cheap, ~thousands of rows).
2. **Panel wiring** (`panel_search.cpp`): after the marker-hit loop, iterate the shop index, resolve
   name via the SAME `lookup_text_utf8` + English alias, token-match the query, and collect "shop hits".
   Render them in a labelled sub-group inside the results child — e.g. `SeparatorText("Sold by merchants")`
   — each row `"{item name}  · buyable{ (unlock required) }"`, with the item icon, **non-selectable**
   (no locate). Dedup against marker hits so an item that is BOTH placed and sold doesn't double-list
   (or: if already a marker hit, just append a "· also buyable" tag to that row instead of a new row).
3. Guard behind a config toggle `merchant_search` (default ON) in the `[Goblin]`/search area; `tr()` the
   new strings (+ fr.txt). No changelog-worthy risk; add a changelog "Added" line.

Delivers: typing a spirit-ash / bell-bearing-mat name surfaces "· buyable" so the player knows it's
purchasable. Does NOT yet name the merchant or pan to it.

### Slice 2 — merchant identity (name the seller) — DEFERRED (user, 2026-07-02)

Group shop rows by merchant and label the row "sold by <merchant>". **Investigated on the live ERR
install (runtime `[SHOPDIAG]` dump of ShopLineupParam row-ids + gated-flag clusters) and DEFERRED —
the naming is blocked without disproportionate RE:**
- The shop-id layout is **ERR-customized**, NOT vanilla: dozens of single-row shops in the
  `57253xxx`–`57291xxx` range, a 42-row block at `58011xxx`, and the flag-gated rows cluster in
  `100201`–`100338` with **ERR-specific unlock flags** (`120xxx`/`130xxx`, e.g. rid 100225→flag
  120250), NOT the vanilla bell-bearing flag range. So **there is no clean Twin Maidens / bell-bearing
  signature** to key on, and a hardcoded vanilla shopId-range table would MISLABEL on this install
  (fails the mod-agnostic acceptance test).
- ShopLineupParam carries no merchant identity; the shop-row → merchant link lives in EMEVD
  `OpenRegularShop(begin,end)` + the triggering talk/**ESD** → NPC entity → NpcName. EMEVD we can
  parse; **ESD is not parsed anywhere** in the repo, so getting the NAME is a new-infra RE spike.

Options if revisited (both non-trivial):
- **(a) shopId-range → merchant reference table** — additive ER layer, but the live data shows ERR
  reassigned ranges, so it needs per-install verification, not a static vanilla table. Fragile.
- **(b) EMEVD OpenRegularShop harvest** (mod-agnostic) — groups rows by merchant RANGE (no name yet);
  the NAME still needs the talk/ESD→NPC hop. Effectively merges with Slice 3.

Slice 1's generic "· buyable (unlock required)" tag stands as the shipped behavior.

### Slice 3 — merchant map pins + locate (optional, heaviest)

Emit a real `Marker` per (sold item, merchant) at the merchant's `entity_world_pos`, so shop items ring
+ locate like any marker. Requires the **shop-row → merchant-NPC-entityId** join: harvest EMEVD/ESD
`ShopOpenDialog`/`OpenRegularShop(begin,end)` events (same style as the portal/summoning EMEVD parse)
to map a shop-id range to the NPC that opens it, then `entity_world_pos`. This is the real RE spike;
do only if the user wants pins.

## Acceptance (mod-agnostic test)

On a DIFFERENT mod with different ShopLineupParam, Slice 1 still lists that mod's merchant stock as
"buyable" (reads live params, no baked/hardcoded shop data). Slice 2(a)'s NAMES may be ERR/vanilla-only
(additive), but the base "buyable" tag must remain correct everywhere.
