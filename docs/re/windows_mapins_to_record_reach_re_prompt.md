# RE prompt — the EXACT MapIns → loot-record reach (last hop for the explore-cache)

> Enumeration is SOLVED and proven in-process: a bounded vtable-scan for `CS::MapIns`
> (`er+0x2a8d6d8`) yields the full loaded set (live: **342** instances, crash-safe). The ONLY missing
> piece is **how to reach the loot record from a MapIns** — and in-process scanning of the MapIns body
> does NOT find it. Need the deterministic field/offset from Ghidra. App = current ERR build; re-anchor.

See `windows_mapins_loot_record_enum_re_findings.md` (the chain + STEP-0 enum) and §7 of
`windows_fieldins_registry_layout_and_preopen_re_findings.md` (the record layout).

## What's proven
- **Enumerate MapIns:** vtable-scan `er+0x2a8d6d8` → 342 live (in-process, `[MAPINS]` walker in
  `goblin_collected.cpp`). (The static chain `WorldMapManImp[er+0x485cbb8]→+0x5d0→+0x110→MapIns` did
  NOT validate live: `[er+0x485cbb8]` holds an object whose vtable ≠ `WorldMapManImp 0x2a8f918` — the
  slot candidate is WRONG. Either re-derive the real WorldMapManImp GetInstance slot, or just use the
  vtable-scan.)
- **The record fields** (from §7): a node `{lotId u32@+0, flag@+4, FieldIns*@+8}` with
  `*(u32)(FieldIns+0x50)==lotId`, `FieldIns` name inline "アイテム"@+0; `MapId@node-0xD8`,
  `localPos@node-0xD4`.

## What FAILS in-process (so we need the deterministic reach)
- Scanning each MapIns body `[+8 .. +0x2000]` for the node signature (lotId in range + flag≤1 +
  `*(P+0x50)==L` + FieldIns name アイテム) finds **0 records**, with the FULL 342 MapIns loaded, and
  **even after opening a chest** (so it's not spawn-time-into-the-body either). ⇒ the node is **NOT
  embedded in the MapIns object body** — da513922's "+0x460" was a session-specific scanned heap offset,
  not a stable MapIns field. The record is in a **child allocation the MapIns points to** (one or more
  pointer hops), OR keyed elsewhere.

## THE ASK — give the deterministic MapIns → record reach
Decompile the MapIns side (ctor `FUN_14071a0f0`, and whatever attaches loot to a map-part — the
`Events.Treasure`/item-gimmick registration that writes the `{lotId,flag,FieldIns*}` node) and answer:
1. **Which field on `CS::MapIns` points to the loot record / the structure that holds it?** A pointer
   at a FIXED MapIns offset (the embedded pool `@+0x240` was EMPTY for the chest — so NOT that). Give
   the offset + the pointer-chain from the MapIns to the node, with the per-hop offsets.
2. **Is it 1 record per MapIns, a list, or a pool?** If a container, its base/count/stride.
3. **Confirm generality** across ≥3 item-bearing MapIns / ≥2 maps (offsets stable, not the 1/343
   one-off).
4. **The join back to position:** confirm `MapId` + `localPos` are reachable from the same record
   (node−0xD8 / node−0xD4), or give their real offsets relative to the record base.
5. If the loot record is genuinely **not reachable from a MapIns by a stable chain** (only findable by
   full-mem scan), say so → then the runtime explore-cache is not feasible from the MapIns anchor and we
   keep baked-only.

## Deliverable — `windows_mapins_to_record_reach_re_findings.md`
The exact pointer chain MapIns → node (fixed offsets, container shape if any), generalised, + the
position fields. Then the in-process `[MAPINS]` walker wires it directly (enumeration already works;
only the per-MapIns reach is swapped in).

Tooling so far: `mapins_enum.py` (342 proven), `lot_pos_scan/lot_obj_dump.py`, `query.java`,
`rtti_index.txt`.
