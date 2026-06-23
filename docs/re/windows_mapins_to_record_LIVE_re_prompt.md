# RE prompt (LIVE process) — pin the MapIns → loot-record pointer chain on the running eldenring.exe

> The game is RUNNING on Windows. Use **RPM on the live process + reverse pointer-scan**, cross-checked
> with Ghidra, to deliver the DETERMINISTIC chain from a `CS::MapIns` to its loot record. In-process
> (Linux DLL) we have proven MapIns enumeration (vtable-scan `er+0x2a8d6d8` = 342 live) and the record
> FIELD layout, but the per-MapIns body does NOT contain the node — so the node is in a child allocation
> reached by a pointer we must pin. App = current ERR build; re-anchor RVAs.

## Known (don't re-derive)
- Enumerate MapIns: vtable `er+0x2a8d6d8` (RTTI `CS::MapIns`). 342 resident.
- Loot record node: `{ lotId u32@+0, flag u32@+4, FieldIns*@+8 }`, `*(u32)(FieldIns+0x50)==lotId`,
  FieldIns inline name "アイテム"@+0; `MapId@node-0xD8`, `localPos vec3@node-0xD4`.
- The MapIns embedded pool `@+0x240` is EMPTY for chests (not the holder).
- Static slot `er+0x485cbb8` is NOT WorldMapManImp (vtable mismatch live) — ignore it / re-derive if needed.

## Method on the LIVE process (bottom-up — most reliable)
1. **Find a live node.** Full-mem scan for a FieldIns with name "アイテム" (`30A2 30A4 30C6 30E0`) whose
   `+0x50` is a plausible lotId; OR scan for a known chest lotId and validate the `{lot,flag,FieldIns*}`
   shape + `*(FieldIns+0x50)==lot`. Note the node address `N` and `FieldIns` address `F`.
2. **Reverse-pointer-scan to a MapIns.** Find address `X0` with `*(X0)==N` (who points at the node).
   Then find `X1` with `*(X1)` inside `X0`'s object, … walk UP until you reach an object whose
   `vtable == er+0x2a8d6d8` (`MapIns`). Record each hop's **field offset** (offset of the pointer within
   its container, and the container's offset within its parent). Cap depth ~4.
3. **Repeat for ≥3 distinct item-bearing nodes across ≥2 maps.** Keep only the offsets that are
   IDENTICAL across all → that's the stable chain (discard the session-specific `+0x460`).
4. **Ghidra cross-check.** Decompile the writer of the node (the `Events.Treasure` / item-gimmick
   registration that stores `{lotId,flag,FieldIns*}`) and the MapIns ctor `FUN_14071a0f0` to confirm
   the pinned offsets are real struct fields, not coincidence. Note the container type (list? pool?
   single ptr?) — give base/count/stride if a container.
5. **Position join.** Confirm `MapId`/`localPos` are reachable from the same record (node−0xD8 / −0xD4)
   or give their real offsets relative to the record/holder base.

## Alternative anchor (report whichever is simpler)
If the node is more cleanly reached from a SINGLETON other than MapIns (e.g. the FD4 field-instance
registry from be1b018, or a treasure/gimmick manager), give THAT chain instead — we just need ONE
stable static-anchor → node path. Report the real `WorldMapManImp` GetInstance slot too if you cross it.

## Deliverable — `windows_mapins_to_record_reach_re_findings.md`
- The stable pointer chain: `MapIns (vt 0x2a8d6d8) +offA -> +offB -> … -> node`, per-hop offsets,
  container shape (single/list/pool + count/stride), generalised over ≥3 nodes / ≥2 maps.
- node field offsets (lotId/flag/FieldIns*) + position (MapId/localPos) relative to a stable base.
- If NO stable anchor→node chain exists (node only findable by full-mem scan) → say so definitively →
  runtime explore-cache from the MapIns anchor is infeasible, keep baked-only.

Tooling present: `mapins_enum.py`, `lot_pos_scan.py`, `lot_obj_dump.py`, `query.java`, `rtti_index.txt`.
The Linux-side `[MAPINS]` walker enumerates + reads crash-safe already; it only needs these offsets.
