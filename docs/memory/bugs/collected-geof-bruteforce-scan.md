---
name: collected-geof-bruteforce-scan
description: "RESOLVED 2026-06-27. Collected refresh Wine/Proton lag fully fixed: read_geof bulk-read (×550) THEN read_wgm + all collected reads dropped ReadProcessMemory for direct in-process deref (clang-cl-safe __try-around-noinline-CALL) → wineserver IPC flood gone, the ~20fps stutter dead. read_wgm 581→4ms."
metadata: 
  node_type: memory
  type: project
---

**✅ FULLY RESOLVED 2026-06-27 (merge 83a723f, branch feat/field-probe-raw-addr).** The
whole collected-refresh Wine lag is dead. Two-stage fix:
1. read_geof brute-scan → bulk-read (×550, 2026-06-22, already below).
2. **The real kill (commit 85cece4): DROP ReadProcessMemory entirely.** `safe_read` /
   `safe_write_byte` (goblin_collected.cpp) were RPM-to-self = a wineserver IPC (~10µs)
   PER read; the per-refresh flood serialises Wine's single-threaded wineserver →
   starves the game thread = constant ~20fps Proton stutter. We're injected INTO
   eldenring.exe → a raw deref reads the same memory for ~ns, no IPC. RPM had ONLY been
   a clang-cl SEH crash-workaround; replaced by the `__try`-around-a-noinline-CALL
   pattern (raw_copy/raw_store8) which clang-cl preserves — see [[clang-cl-seh-noinline]]
   (its "RPM is the only fix" verdict is now superseded). VERIFIED in-game (16:58 [BENCH]):
   read_wgm total 2012→37.7ms, avg 6.99→0.13, **max 581→4.09ms**; refresh.collected.total
   2124→103ms. Beats the chunked-cold-load idea (kills the cost, not just spreads it).
3. Also landed: goblin_field_probe **raw-address mode** (`@geom` spec, arm_raw + DR0 on a
   live CSWorldGeomIns alive byte @ +0x263/+0x26B) to still RE the native O(1)
   collected-getter later (prompt: docs/re/windows_geom_collected_getter_re_prompt.md) —
   now an optimisation, not a necessity. Residual: confirm the SEH catches faults under
   region-traversal/eviction churn (a freed-block deref crash = the only failure mode).

**HISTORY below (2026-06-22 read_geof investigation) — kept for the RPM-anti-pattern lesson.**

**Surfaced 2026-06-22** by the new aggregate [BENCH] session report ([[overlay-rendered-markers]] sibling work; report = commit b3a3260, dumps every 30s since DLL_PROCESS_DETACH never fires under Proton). Live numbers from a 47s session:

| label | calls | avg | max | %wall |
|---|---|---|---|---|
| refresh.collected.total | 115 | 163ms | 793ms | **39.9%** |
| refresh.collected.read_geof | 115 | **153ms** | 236ms | **37.5%** |

`read_geof` dominates the whole DLL CPU budget. Everything else cheap (census/flag_or_pairs ~0ms; read_wgm 9.5ms — already has a perf cache @ goblin_collected.cpp:253). One-time inits (init.from_params 9s, init.param_poll 5s) are startup-only.

**ROOT CAUSE = brute-force scan (NOT an AOB scan — that's modutils::scan, 1×/init).** `read_geof_from_memory` (goblin_collected.cpp:497) → `read_singleton_entries` (line 150):
```c
for (int off = 0x08; off < 0x20000; off += 16)   // 128KB region, stride 16 = up to 8192 iters
{ safe_read(gf_ptr+off, &id_val, 8); safe_read(gf_ptr+off+8, &ptr_val, 8); ... }
```
= up to **~16,000 ReadProcessMemory calls per refresh** (2 per 16-byte slot), + per valid tile a header read + `count` entry reads. `safe_read` = `ReadProcessMemory(GetCurrentProcess(),…)` — used for clang-cl's non-elidable SEH ([[clang-cl-seh-noinline]]), but RPM is a **kernel call marshaled by Wine** → very expensive under Proton. 16K × that = the 153ms. The scan is a HEURISTIC (no known table size): `consecutive_empty>256` early-out, area-byte 0x0A..0x3D + ptr-range sanity filters.

**⚡ WINDOWS-vs-LINUX PROOF (user 2026-06-22):** on NATIVE WINDOWS read_geof shows ~ZERO lag; on Linux/Proton ~37.5%/153ms. This NAILS the cause = per-call ReadProcessMemory cost, NOT the parse work. RPM on GetCurrentProcess() is a cheap guarded-memcpy kernel path on Windows (sub-µs → 16K calls free), but under Wine each RPM is a wineserver IPC round-trip (~10µs × ~16K = ~150ms). So the lever is the RPM CALL COUNT × the Wine per-call tax. → bulk-read (fewer, bigger RPM) is a no-op on Windows but ~×500 fewer wineserver round-trips on Linux = the lag vanishes; zero Windows regression. Same anti-pattern anywhere else will ALSO be Linux-only lag invisible on Windows — watch for it.

**✅ RE ANSWERED + FIX IMPLEMENTED (2026-06-22, NOT in-game verified yet):**
- RE findings `docs/re/windows_geom_flag_savedata_table_re_findings.md` (commit 6d41b0e): the +0x08 table = a Dantelion2 **DLFixedVector** — inline contiguous, capacity **6300**, stride **0x10** `{u32 tile_id; u32 pad; void* block}`, sorted by tile_id, LIVE count = u64 @ **manager+0x189d0**. Per-tile block = the old "Layout A" (count@+0x08, entries@+0x10, stride 8); "Layout B" never fires. Record bit-pack `((geom_idx|model_id<<17)<<15)|present` → `p[1]=(geom_idx&1)<<7` explains the 0x00/0x80 filter (existing decode byte-correct). Mutation: block ptrs realloc on load/unload + vector shifts on insert/evict → known-blocks cache UNSAFE; full bulk re-read each refresh = correct. Function-RVA map in the doc.
- **Bulk-read SHIPPED (commit 145a41a):** rewrote `read_singleton_entries` (goblin_collected.cpp) → read count@+0x189d0, 1 bulk RPM of count*0x10, 1 bulk RPM per tile's entry block. ~16K RPM → 1+2·(#tiles). Decode preserved byte-for-byte. Per-element fallback if a bulk read straddles an unmapped page. Built + deployed md5 6d2c3d.
- **✅ VERIFIED IN-GAME (2026-06-22, [BENCH] re-run):** read_geof avg **153.37ms → 0.28ms** (total 17637ms → 75ms; %wall 37.5% → 0.16%; max 236ms → 4.58ms) over 271 refreshes — **≈×550 faster, the Linux cliff is dead.** collected.total 163ms/39.9% → 10.5ms/6.08%. Collected graying still correct (set effectively identical). DONE.
- **NEW dominant refresh cost = `read_wgm`** — and it was the SAME RPM-wineserver cliff. Windows-vs-Linux proof (same code): Linux avg **11ms / max 961ms** (6% wall) vs Windows avg **0.29ms / max 7.9ms** (0.19%) = **38× / 120× spike**. Runs EVERY refresh (gameplay + map) → this is the "lag hors-worldmap aussi" the user felt; the 961ms cold-cache spikes = the felt micro-freeze on tile-load (wineserver is single-threaded → an RPM storm on the refresh thread stalls the render thread too).
  - **FIX SHIPPED (commit 5bef273, NOT in-game verified):** bulk-read the RPM chains, decode byte-identical. Warm path: both alive flags (+0x263/+0x26B) as one 9-byte span (2→1 RPM/inst). Cold path (the spike): bulk the geom_ins pointer vector in 1 RPM + per-inst bulk the geom_ins header (0..0x26C → msb_part@+0x48 + both flags) + the MSB-part header (0..0x2C → name ptr + 3 pos floats). ~9 RPM/inst → ~3. Per-field fallback on faulted bulk. Built+deployed md5 d7df8c. VERIFY: read_wgm max should drop well below 961ms; graying still correct.
  - Floor without restructuring = 3 RPM/inst (geom_ins blob + msb_part blob + name string — name is a separate alloc, needed to filter AEG family). Further wins would need cross-tile caching or a different strategy.
- **[BENCH] tooling added:** `GOBLIN_BENCH_QUIET` (aggregate-only, no per-line spam — for hot per-frame paths) + a `render.worldmap` timer (commit 64d2e3c) proving the per-frame overlay render (~11ms avg, 0.97%, map-only) is SECONDARY to read_wgm.
- **⚠️ THE ACTUAL worldmap lag = a dev DUMP flag left ON in the user's INI (2026-06-22), NOT read_wgm/render.** User reset config → lag gone. Root: the `dump_converters`/`dump_native_pins`/`dump_icon_textures` flags run on the 100ms `probe_loop` thread, RPM-heavy (std::map/converter walks) → under Wine's single-threaded wineserver, the RPM storm stalls the RENDER thread too = real map lag with NO [BENCH] line (the report pointed at read_wgm/render, both real-but-secondary). **[BENCH] had a blind spot: debug/dump paths weren't instrumented.** FIXED (commit 202a8fc): wrapped the 3 probe_loop dumps in GOBLIN_BENCH_QUIET → show as `debug.dump_*` in the report (zero cost when off). Render-path debug (debugRegionVolumes/clusterDebugRadius) is already inside `render.worldmap`. LESSON: when a [BENCH] report doesn't explain felt lag, suspect an uninstrumented config-gated debug/dump flag BEFORE micro-optimising the benched paths. The session's read_geof/read_wgm/render-cull wins are still valid (all reduce real Wine-RPM cost), just weren't THE lag. Remaining uninstrumented: the game-event hook debuggers (debugEventFlags/debugItemGrants/debugFlagCapture @ dllmain.cpp:241) — bench later if a hook-debug flag is suspected.
- **STILL UNINVESTIGATED — user suspicion: the GPU item-icon images** ("regression appeared after adding them"). The GPU texture copies / fence-waits (DXVK) are NOT in [BENCH] and don't run every frame → need a separate probe (wrap the icon-harvest / GPU-copy / fence-wait path in GOBLIN_BENCH_QUIET). Pursue ONLY if lag persists after the read_wgm fix lands. See [[overlay-icon-atlas]] (icon harvest), commit 27f9eea (fence-batching).

**FIX PLAN (historical / remaining):**
1. **Bulk-read** the outer `{id,ptr}` table at `gf_ptr+0x08` — ONE (or a few page-aligned 4KB chunk) RPM into a local buffer, parse in-memory → 16K syscalls → ~1–32. Caveat: RPM fails whole-range if any page unmapped → read by aligned 4KB chunks, skip faulting chunks. The `ptr_val` heap allocs stay per-tile reads (few valid tiles → negligible).
2. **RE the real GeomFlagSaveDataManager table bounds** so the scan need not guess 0x20000 — find the actual count/capacity field on the manager object to size the read exactly (kills the 128KB over-scan). Manager slot resolved by AOB (goblin::sig, geom_flag_slot()). NOTE: GeomNonActiveBlockManager is deliberately NOT scanned (different 0x820 layout, see goblin_collected.cpp:507 + docs/geom_nonactive_block_manager.md).
3. Optional cheap win: **throttle** read_geof to ~1s instead of every 100ms tick (collected graying isn't frame-critical) — independent of 1/2.

**User idea (2026-06-22) — "scan once, cache the flags/addresses, replace the scan":** valid only as a HYBRID, NOT a one-shot cache. The GeomFlagSaveData table MUTATES while playing (collecting a node ADDS an entry — new tile slot, or `count` grows on a known tile; vector growth REALLOCS the per-tile `ptr_val`). A frozen cache → misses newly-collected items (graying stops updating) + stale realloc'd ptrs read garbage. Workable hybrid: cache the discovered slot OFFSETS, fast-path re-read just those slots each refresh (re-read the slot to get the CURRENT ptr → then its block, handles realloc), full outer re-scan only periodically / on new-tile load. BUT this adds invalidation complexity for ~the same gain as the simpler, zero-correctness-risk bulk-read (which reads the same FRESH data, just in few big chunks). → prefer bulk-read + table-bounds RE; keep the hybrid as fallback if bulk-read alone isn't enough.

Same RPM-per-small-read anti-pattern may exist elsewhere — the [BENCH] report is now the tool to spot the next one. See [[overlay-rendered-markers]].
