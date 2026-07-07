# ESD / EzState blocker — the command-argument decoder (RE'd + the 78/21 split)

**Status: the READ blocker is CRACKED for the common case (2026-07-07).** ESD command arguments are EzState
bytecode expressions, not plain int32 — that was the wall the merchant-pin spike hit
(`docs/plans/merchant_item_search_plan.md` Slice 3, `[[grace-menu-esd-spike]]`). This measures the wall and
breaks the 78% literal case with a ~10-line decoder, so shop-id ranges / textIds / event-flag ids are now
readable. The remaining 21% (branching expressions) need a full EzState disassembler (a bounded port of
SoulsFormats' EzSemble), NOT novel RE. ERR/ERRv2.2.9.6 talk ESD; tool `tools/esd_shop/` (Andre.SoulsFormats).

## What the blocker actually is

The `tools/esd_shop/` reader (Andre.SoulsFormats.ESD) already exposes the ESD STRUCTURE — `StateGroups →
State.{Entry,While,Exit}Commands + Conditions`, `CommandCall.{CommandBank, CommandID, Arguments}` (banks:
1 = talk commands, 5 = text/FMG, 6 = ESD function calls, 7 = getters). The wall is that each argument in
`CommandCall.Arguments` is a **byte[] of EzState bytecode** — a little stack-machine expression, not an
int. Andre.SoulsFormats has NO EzState decoder (`EzSemble`/`EzInfo` absent — grep = 0). The old esd_shop
"decode" read the first 4 bytes of the expression, which INCLUDES the `0x82` push opcode → garbage
(`0x00004B82` instead of `75`).

## The EzState bytecode (measured on `m00_00_00_00.talkesdbnd.dcx`, 50 445 command args)

- **78% are a single literal:** `82 <int32 LE> A1` — `0x82` = push-int32, 4 LE bytes, `0xA1` = end. E.g.
  `82 4B 00 00 00 A1` = `75`; `82 FF FF FF FF A1` = `-1`. Decoding = read `int32` at offset 1. **Done.**
- **21% are stack-machine expressions** — still a small, documented opcode set: pushes (`82`, plus float
  pushes), binary/compare/logic operators (`86 89 95 99 85 …`), and function calls (`6F …`). Example
  (a condition-style expr): `6F 82 03 00 00 00 82 FB 00 00 00 … 89 … 95 … 99 A1`. These need a real
  disassembler/evaluator.
- First-opcode histogram over all args: `82`×43123, `40`×3804, `4F`×1130, `41`×858, `50`×218, `42`×187,
  `4A`×84, `81`×73, `6F`×63, `43`×57 (the `0x40–0x50` band = operators; `0x6F`/`0x81` = calls/gets).

## What the literal decoder already unblocks (live-proven)

`esd_shop dump 1:19` (AddTalkListData = a selectable menu line) now reads correctly:
```
1:19  args=[75, 71000000, -1]      # index 75, textId 71000000 (menu FMG id), iconId -1
1:19  args=[10, 50000051, -1]      # …
6:2147483643  args=[15000560]      # an ESD function call carrying event-flag id 15000560
```
So menu/dialogue **textIds** and **event-flag ids** (the load-bearing "does this option set/read a flag"
the spike couldn't trace) are now extractable, as are literal **shop-id ranges** (the merchant-pin join's
missing operand — Slice 3). Any ESD command whose args are literal constants is now readable.

## What it does NOT solve (the genuinely hard/strategic parts — unchanged)

1. **The 21% branching expressions** (Condition.Evaluator logic, computed args) → port SoulsFormats'
   **EzSemble** disassembler (~500 lines C#, well-documented opcode set; mainline SoulsFormats has it, the
   Andre fork doesn't). Bounded work, not RE. Needed for full talk-flow understanding, not for literal reads.
2. **Authoring / injecting NEW ESD** (the write side) → needs the EzSemble ASSEMBLER too, AND ERR re-ships
   the 524 KB `talkesdbnd` every version, so any edit is a build-time patch that breaks each ERR update
   (`[[grace-menu-esd-spike]]` "strategically POOR"). Prefer a DLL-side approach (read flags the ESD sets;
   own overlay) over patching ERR's file.
3. **A runtime EzState evaluator in the DLL (C++)** for mod-agnostic LIVE talk behavior → large; mostly
   avoidable by reading offline + baking, or the runtime-capture route (merchant plan option C).

## Recommendation

- **Cheap + high-value, do first if pursuing merchant pins:** use the literal decoder to extract the
  shop-open command's shop-id range (find the bank:id via `esd_shop hist` on a merchant map, `dump` it),
  then the shopRange→TalkID→NPC-entity join → `entity_world_pos` (Slice 3's blocked step).
- **Medium, unblocks all static ESD analysis:** port EzSemble's disassembler into `esd_shop` for the 21%
  (condition logic, flag comparisons). One-time, mod-agnostic offline.
- **Avoid** patching ERR's talkesdbnd (write side) — same verdict as the 2026-06-18 spike.

## Tooling

`tools/esd_shop/` (net10; build net9 works via a `TargetFramework` swap). `hist` = bank:id histogram;
`dump [bank:id]` = EzState-decoded args (literal ints; `<expr:…>` for the 21%); `raw [bank:id]` = raw arg
hex. Talk ESD is DFLT/zlib (not Oodle) → reachable offline on Linux + Windows. `DecodeArg` in `Program.cs`
handles the literal `82 <i32> A1` form; extend it with an EzSemble port for full coverage.

## ★ Merchant-pin join — RE'd + PROVEN end-to-end (2026-07-07, the Slice-3 spike, now unblocked)

With the literal decoder, the shelved merchant-pin join is fully solved offline:
- **`OpenRegularShop` = talk command `1:22`**, args `[shopBegin, shopEnd]` (a ShopLineupParam id range).
  RE'd by cross-referencing: over all ERR talk ESDs, `1:22` is the only command whose two literal args are
  consistently real `ShopLineupParam` rows ≥100000 (every instance is a clean shop range: Kalé
  `[100650-100674]…`, Corhyn `[100350-100399]`, etc.). Other bank-6/1 candidates carried event flags/menu
  ids, not shop rows (e.g. m11_10's `701010`/`609000` are NOT ShopLineupParam rows).
- **The join** (`tools/esd_shop/merchant_join.py`): `t<TalkID>.esd 1:22 → shop range`, then the vanilla
  MSB `Parts.Enemies` (each has `TalkID`, `EntityID`, `Position`, `NPCParamID→NpcName`) joined on `TalkID`.
  **487 placements → 39 unique merchants** (drop `talkId 1000` = the DLC scaling dummy; dedup the _00/_10
  LOD tile pair): Merchant Kalé, Twin Maiden Husks (the bell-bearing multi-shop), Enia, Hewg, Sellen,
  Patches (×4 locations), Corhyn, Bernahl, Miriel, Seluvis, Gowry, Thops, Rogier, Pidia, Iji, Thiollier,
  Ymir… — all with correct per-tile MSB-local positions (feed the existing marker projection like any
  marker) + shop ranges (feed the existing ShopLineupParam→items index for the stock).
- **Tools:** `esd_shop` (ESD decode) + `merchant_join.py` (pythonnet: ERR `regulation.bin` NpcParam +
  vanilla NpcName FMG + vanilla MSBs). `merchant_join.py --json` emits `merchants.json` (39 rows). This IS
  the offline pipeline for a baked merchant layer (option B) and validates the data any runtime path needs.
- **Remaining to SHIP pins = an architecture choice (plan Slice 3 A/B/C), then marker wiring** — NOT more
  RE. (A) runtime C++ ESD parse = mod-agnostic but big; (B) bake `merchants.json` = fast, ERR-frozen
  (positions are vanilla map data, fairly mod-stable; a mod ADDING merchants wouldn't appear); (C) runtime
  shop-open hook = visited-only. The join is proven; pick A/B/C before wiring.
- **RESOLVED 2026-07-07: (A) SHIPPED.** `src/worldmap/esd_parser.cpp` is the C++ port of the SoulsFormats
  reader (oracle-exact vs `esd_shop`: 161/161 literal 1:22); `tools/esd_cpp_test/` re-runs both oracle
  compares offline. The runtime join found ERR's OWN merchant edits the vanilla-MSB `merchants.json`
  can't see (Kalé replaced by talk 437006001, +3 ERR-added merchants) — validating the mod-agnostic
  choice. See `docs/plans/merchant_item_search_plan.md` for the shipped wiring.

Related: `[[grace-menu-esd-spike]]` (menu mechanism: AddTalkListData 1:19, open 1:20, show 1:10),
`docs/plans/merchant_item_search_plan.md` Slice 3 (the merchant-pin join, now RE-complete).
