# Merchant / shop item search — plan

Status: **Slice 1 SHIPPED. Slice 3 join is now RE-COMPLETE + PROVEN (2026-07-07) — only an
architecture choice + marker wiring remain, NOT more RE.** The 2026-07-03 "needs an EzState EVALUATOR,
disproportionate" verdict is superseded: `docs/re/esd_ezstate_decoder_re_findings.md` shows 78% of ESD
args are the literal `82 <i32> A1` form, and the full join is proven end-to-end:
- **`OpenRegularShop` = talk command `1:22`** (args `[shopBegin, shopEnd]`), RE'd by cross-referencing
  every `1:22` arg pair against real ShopLineupParam rows.
- **`tools/esd_shop/merchant_join.py`** joins `t<TalkID>.esd 1:22` → shop range with the MSB
  `Parts.Enemies` (TalkID→EntityID+Position+name) → **39 unique merchants** (Kalé, Twin Maidens, Enia,
  Hewg, Sellen, Patches×4, …) with correct positions + shop ranges. `--json` emits `merchants.json`.
- **NEXT = the A/B/C fork below (needs a user call), then wire a marker layer.** Positions are MSB-local →
  reuse the existing marker projection; shop ranges → reuse the shipped ShopLineupParam→items index. Verified on ERR/Proton: `[MERCHANTSEARCH] 5485 items indexed` at boot; F1 search "telescope"
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
+ locate like any marker. Requires the **shop-row → merchant-NPC-entityId** join, then `entity_world_pos`.

**SCOPING DONE 2026-07-03 — the join source is ESD, NOT EMEVD.**
- **EMEVD is a DEAD END.** ER EMEVD has NO shop-open instruction — verified by parsing
  `tools/er-common.emedf.json` (27 classes, no shop/buy/sell/purchase/OpenRegularShop; only
  `2003[78] Open World Map Point`). "OpenRegularShop" is DS1 terminology; ER opens shops from **talk
  ESD** (EzState), so the earlier "EMEVD OpenRegularShop(begin,end)" premise was wrong. The existing
  EMEVD parsers (portals 90005605, quest-NPCs 90005702 in `msbe_parser.cpp`) can't reach it.
- **ESD is the only source, and it IS reachable on Linux** (talk ESD is DFLT/zlib, not Oodle):
  `~/Games/ERRv2.2.9.6/mod/script/talk/mXX_..._.talkesdbnd.dcx` on disk; the SoulsFormats.ESD reader
  ships as `tools/lib/Andre.SoulsFormats.dll` (committed) + dotnet 10 available. Precedent: the
  quest-browser mined talk ESD (`[[quest-browser]]`, `docs/emevd_death_flags_results.md`). **Caveat:
  the `tools/esd_dump/` C# SOURCE is NOT committed (only `bin/`)** — it must be rebuilt (small C# using
  Andre.SoulsFormats: `ESD.Read`, `StateGroups`→`State.{Entry,While}Commands`→`CommandCall.{CommandBank,
  CommandID,Arguments}`; bank 1 = talk commands, see `[[grace-menu-esd-spike]]`).
- **The join chain:** talk ESD command `OpenRegularShop(begin,end)` (a bank:id to FIND via
  `esd_dump --hist` on a merchant map, e.g. Roundtable `m11_10`) → gives the shop-id RANGE per talk
  script → TalkID → NPC entity (MSB part talkId / `tools/_npc_talkids.py`) → `entity_world_pos`.

**DECISION FORK (prime-directive tension) — get a user call before building:**
- **(A) Runtime ESD parse (mod-agnostic, BIG):** a new in-DLL EzState/ESD parser reading the ACTIVE
  install's `talkesdbnd.dcx` at boot (DFLT decompress already exists). Correct on any mod. Large new
  infra (ESD state machines in C++), the heaviest option.
- **(B) Baked-additive table (FAST, ERR-frozen):** offline `esd_dump` pipeline → `data/merchants.json`
  (shopRange→entity→pos), loaded as an additive ERR layer. Violates the mod-agnostic acceptance test
  (wrong under other mods) → a transitional fallback per the prime directive, not the end state.
- **(C) Runtime shop-open HOOK (lightweight, partial):** hook the game's shop-open fn; when the player
  opens a merchant, capture (shopId range, talking-NPC entity) live → pin progressively. No ESD parse,
  mod-agnostic, but only pins merchants ALREADY VISITED (weak for a "find merchants" map).

Recommendation: (A) is the only true-to-directive end state but is a large spike; (C) is a cheap
partial win; (B) is quick but frozen. Do the `esd_dump` rebuild + `--hist` RE FIRST (bounded) to
confirm the shop command + prove the join on one merchant, THEN pick A/B/C. This is the real RE spike;
do only if the user wants pins.

**RE SPIKE DONE 2026-07-03 — cost ESCALATED, lean toward (C) or shelve.** Rebuilt the ESD reader as
`tools/esd_shop/` (Andre.SoulsFormats, dotnet 10 — works on the ERR `talkesdbnd.dcx`, histogram +
command dump). Findings on Roundtable `m11_10`:
- The reader works (banks confirmed: 1 = talk, 5 = text, 6 = ESD function calls — matches
  `[[grace-menu-esd-spike]]`), BUT **ESD command ARGUMENTS are EzState BYTECODE EXPRESSIONS, not plain
  int32** (decoded values are `0x7FFFFFA7`-style sentinels + recurring EzState constants 386/130). So
  reading the command is easy; extracting its shop-id RANGE operands needs an **EzState bytecode
  evaluator** — new infra on TOP of the ESD reader, needed by BOTH (A) and (B).
- Net cost for real pins now = ESD reader + **EzState evaluator** + identify the OpenRegularShop talk
  command id + shopRange→TalkID→NPC-entity→`entity_world_pos` join. That is disproportionate for ONE
  map-pin category (the plan's original Slice-2 "disproportionate" call now extends to the pins).
- **Revised recommendation: (C) the runtime shop-open hook** (hook the game's shop-open fn, capture
  (shopId, talking-NPC entity) live, pin progressively) — needs ZERO ESD/EzState work, is mod-agnostic,
  and reuses `entity_world_pos`; accept "pins appear once you've met the merchant." OR **shelve pins**
  and keep Slice 1's search (already shipped) as the merchant feature. Do NOT build A/B unless a real
  need for full-static merchant pins justifies the EzState-evaluator infra.

## Acceptance (mod-agnostic test)

On a DIFFERENT mod with different ShopLineupParam, Slice 1 still lists that mod's merchant stock as
"buyable" (reads live params, no baked/hardcoded shop data). Slice 2(a)'s NAMES may be ERR/vanilla-only
(additive), but the base "buyable" tag must remain correct everywhere.
