# RE prompt — find the native O(1) "is this geom collected/open" getter

## Goal

Find the game's **own** function that answers *"is this placed geometry asset
(rune/ember piece, gather node, statue, …) already collected / opened?"* by id —
an **O(1) native getter** we can AOB-pin and **call in-process**, the way
grace-discovered already calls `IsEventFlag` (`IS_EVENT_FLAG` AOB →
`goblin::ui::read_event_flag`, no RPM, ~ns per call).

This replaces `read_wgm_snapshot()` (`src/goblin_collected.cpp:352`), which walks
the `CSWorldGeomMan` RB-tree + per-instance MSB-part chain + 64-wchar name per
loaded tile. On Wine/Proton every read is a `ReadProcessMemory` wineserver IPC
(~10µs); a cold tile streaming in does ~200+ RPM in one refresh tick →
**581ms spike** = the in-game micro-lag when crossing into a new region
(`[BENCH] refresh.collected.read_wgm max 581ms`). Native Windows doesn't feel it
(RPM-to-self is a cheap kernel path); this is a Wine-IPC problem.

**Why a getter, not "more bulk-read":** the steady state is already cheap
(per-tile cache, re-reads only the 9-byte alive span). The cost is the **cold-load
enumeration**. A by-id native predicate we call in-process (no RPM, no enumeration)
collapses the whole snapshot to N cheap calls — one per tracked marker — and kills
the spike. Our marker already carries everything an id-based getter could need:
`tile_id`, `geom_slot`/`geom_idx`, `model_id`, part name, world pos.

## What we already know (don't re-derive — anchor from here)

Static RE already mapped the STORAGE; this prompt is about the **consumer/getter**,
not the table. Background:
- `docs/re/windows_geom_flag_savedata_table_re_findings.md` — `GeomFlagSaveDataManager`
  = `DLFixedVector` of `{u32 tile_id; u32 pad; void* block}` (cap 6300, count
  @`+0x189d0`), sorted by `tile_id`; `lower_bound` = `FUN_1406ea190` (RVA `0x6ea190`).
  Per-tile entry block: count@`+0x08`, `Entry[]`@`+0x10` stride 8; record =
  `((geom_idx | model_id<<17)<<15) | present`. GEOF bulk-read recipe already shipped.
- `docs/re/geom_collection_tracking.md`, `docs/re/geom_nonactive_block_manager.md` —
  GEOF (unloaded) vs WGM (loaded) split.
- `src/goblin_collected.cpp`: `geom_flag_slot()` (AOB `GEOM_FLAG_SLOT`, was RVA
  `0x3D69D18`), `world_geom_man_slot()` (AOB `WORLD_GEOM_MAN_SLOT`, was RVA
  `0x3D69BA8`). `CSWorldGeomMan` RB-tree size @ `WGM+0x18+0x10`. `CSWorldGeomIns`:
  alive flags @ `+0x263` / `+0x26B`, header size `0x26C`; MSB-part header `0x2C`.
- `tile_id = area<<24 | gridX<<16 | gridZ<<8` (`encode_tile()`).
- AOBs live in `src/re_signatures.hpp` (`GEOM_FLAG_SLOT`, `WORLD_GEOM_MAN_SLOT`,
  `IS_EVENT_FLAG`); resolved + logged `[SIG]` at init.

Ghidra project: `D:\ghidra_proj2\ER` (eldenring.exe, imagebase `0x140000000`).

## We have an in-DLL "who reads at this address" tool — USE IT

`src/goblin_field_probe.{hpp,cpp}` (`goblin::field_probe::initialize`, config
`probe_field_access`): sets a **hardware breakpoint (DR0)** on a live address and a
vectored handler logs ONLY the accessing instruction whose RIP is inside
`eldenring.exe` (every mod read is auto-skipped). It cracked the param-offset
source-of-truth AOBs (goodsType, sortGroupId, AEG `pickUpItemLotParamId`). It is
**currently param-row specific** (resolves the address via `get_param(row)+offset`).

**Method A (dynamic, preferred):** extend the probe to arm DR0 on an **arbitrary
geom address** — a resolved `CSWorldGeomIns` instance's alive byte (`+0x263` /
`+0x26B`) for a known placed asset, OR a live `GeomFlagSaveDataManager` entry byte.
Then trigger the game's own read: load the tile, approach/interact with the asset,
or open the area. The `[FWA]` hit RIP is the game **reading the collected bit** —
walk up to the enclosing function = the getter. Capture its entry, args in
registers at the call, and a byte window for the AOB.

**Method B (static):** xref the known storage functions — callers of `lower_bound`
`FUN_1406ea190`, and consumers of `CSWorldGeomMan` (`WORLD_GEOM_MAN_SLOT`) and the
`CSWorldGeomIns` alive flags `+0x263/+0x26B`. Find the **public predicate** the
engine calls to decide whether to spawn/hide a collected asset (likely near asset
activation / `CSOpenGameFlag` / `EzState`/EMEVD `IsObjActive`-style checks). Map it.

## Deliverables

1. **The getter function**: RVA + a unique **AOB** (for `re_signatures.hpp`), and
   confirmation it is patch-stable (wildcard rip-disps / rel8s).
2. **ABI / signature**: exact inputs and return.
   - Does it take an **entityId**, a **(tile_id, geom_idx)**, a **CSWorldGeomIns\***,
     or a manager+key? Which of those do we already have on the marker?
   - Return type/meaning (bool / u8 / flag). Does "collected" = a set bit, a missing
     instance, or a GEOF present-flag? Reconcile with our `flags==0x00|0x80` decode.
   - Calling convention, which manager singleton it needs as `this` (if any).
3. **Coverage**: does ONE getter cover BOTH loaded (WGM) and unloaded (GEOF) tiles,
   or are there two? We need the collected state for assets in tiles the player is
   NOT currently standing in (map overlay shows the whole world). If the getter only
   works for loaded tiles, say so — then GEOF bulk-read stays for the rest and the
   getter only replaces the WGM path.
4. **Call-safety**: can we call it **in-process** like `read_event_flag`?
   - Must it run on the **game thread** (our collected refresh is a background
     thread — `dllmain.cpp:296`), or is it reentrant/thread-safe to call from the
     poll thread?
   - Any locks it takes, any state it mutates (it must be a pure query — must NOT
     mark things collected or trigger spawn). Flag any side effects.
   - Null/unloaded-input behavior (must fail gracefully, not fault).
5. **Perf sanity**: confirm N in-process calls (N = tracked collectibles, ~1546:
   1244 Rune + 302 Ember pieces) with no RPM beats the current snapshot. Estimate
   the per-call cost.

## Validation (needs the running game — quentin)

1. Resolve the getter by AOB (not static RVA — ASLR). `[SIG]` PASS.
2. For a handful of assets with KNOWN state (one collected, one not), call the
   getter and diff against the current `read_wgm_snapshot` / GEOF result — must
   match exactly.
3. Re-run `[BENCH]`: `refresh.collected.read_wgm` should collapse (no RB-tree walk,
   no per-instance RPM) and the 581ms cold-load spike should vanish on Proton with
   no change to Windows correctness.
4. Visual: collected rune/ember pieces still gray/check correctly after crossing
   tile boundaries, with no micro-lag.

## Notes

- Keep `IsEventFlag`/`read_event_flag` as the precedent for the in-process call
  pattern (resolve slot+fn by AOB, call with the right `this`, no RPM).
- If Method A needs the probe generalized (arm on a raw address instead of a param
  row), that extension is in scope — it's a small change to `goblin_field_probe`
  and is reusable for future geom/runtime RE.
- If NO by-id getter exists (the engine only ever bulk-applies GEOF on tile load),
  say so explicitly — then the fallback is the chunked cold-load resolution
  (budget instances/tick) rather than a getter.
