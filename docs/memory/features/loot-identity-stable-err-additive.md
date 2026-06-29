---
name: loot-identity-stable-err-additive
description: "ERR is ADDITIVE (adds items), not a swap-randomizer — loot identity doesn't drift. SUPERSEDED 2026-06-27: ERR loot is now FULLY off-bake (no-bake Phase 2 done, baked=0) — position came off-bake too via the DiskMSB parser + live recovery passes, not the deferred runtime chase"
metadata: 
  node_type: memory
  type: project
---

**UPDATE 2026-06-27 — LOOT NOW FULLY OFF-BAKE (ERR).** The "position = last baked loot field,
feature DEFERRED" conclusion below is SUPERSEDED. No-bake Phase 2 shipped (older note: HEAD `4a7716d`;
import checkout 2026-06-28 is `master@931438d`):
ERR `MAP_ENTRIES` is empty, **baked residual = 0**. Position did NOT stay baked — it came off
the live `DiskMSB` MSBE parser (block-local pos → world via gridXZ·256+local) plus live
recovery passes for the non-MSB residual (EMEVD enemy-death awards, sequence-sibling walks,
bossEntity@16, cross-tile LOD, merchant-phantom via ShopLineupParam). The runtime FieldIns/
MapIns chase documented at length below was NOT the path that won — the DiskMSB parser was
(see [[msbe-parser-supersedes-bake]]). Final provenance: disk 8381 / live 469 / live-cls 74.
The §8 / FieldIns / MapIns history below is retained as RE record only. (Vanilla/ERTE/
Convergence variants still baked — ERR only.)

In-game probe (2026-06-23, `diag_loot_flags` → `[LOOTID]`): lot-backed loot identity
baked-vs-live = **4316 total, 4235 same (98%), 81 "drifted"**. The 81 are NOT real item swaps —
all are baked == live **+ exactly 100000000** = a dormant `encode_live_item` cat-2 **ammo**
encoding quirk (generator adds +100M to all cat-2; encode_live_item skips it for id≥50M). It had
no callers before the probe → no shipping impact.

**Key facts:**
- **ERR is ADDITIVE** — it ADDS items/loot, does NOT shuffle existing vanilla lots. So existing-lot
  **identity does not drift** (the 98%). The boss bake was the same story: bake wasn't the problem.
- The drift that matters for an additive mod is **COVERAGE** (live `ItemLotParam` lots/items with
  no baked marker after ERR ships NEW content), NOT identity. Our bake already includes ERR's added
  items as of the extracted regulation; the gap only opens when ERR adds content + we haven't
  re-extracted. A coverage diff would catch that — the identity path would not.
- **UPDATE: loot identity is now read LIVE + the baked textId1 stripped (shipped to master).**
  Despite ~0 drift, the user chose runtime-by-design (cleanest = runtime). `resolve_loot_item_textid`
  (ItemLotParam slot-1 id@+0x00 cat@+0x20 → encode_live_item) drives BOTH the marker label AND the
  setup_messages FMG name-preload (must agree, else no name at draw — the key gotcha). Baked
  `.textId1` removed from all 4316 lot-backed rows in goblin_map_data.cpp + generate_data.py skips
  it; non-lot rows keep baked textId1 (no live source). `probe_loot_identity` removed (obsolete).
  ⚠️ Prereq that made it safe: fixed `encode_live_item` cat-2 to always +100M (the canonical
  baker/icon/name convention; was `>=50M?raw`, the dormant ammo quirk) — probe then 4316/4316 same,
  0 drift, proving live==baked before the strip. Verified in-game zero regressions.
- **Loot POSITION = the only loot field still baked — RE NOW FULLY VALIDATED, feature DEFERRED**
  (commits a5ac49b/3fb1540/561b00e, other session, 2026-06-23). (1) Offline pre-check: re-extracted
  items_database from current ERR MSB, diffed vs bake = **0 of 30305 placements moved, 0 added/removed,
  byte-identical** → zero drift. (2) RE solved end-to-end: the asset-placement read is ALREADY running
  in goblin_collected.cpp — CSWorldGeomMan → BlockData rb-tree → geom_ins vec @BlockData+0x288 →
  MsbPart @+0x48 → name@+0x00 + pos@+0x20; join partName → items_database → itemLotId → marker (T2
  enemy-drop lotType 2 still needs the abandoned WorldChrMan enum, geom covers map lots only). (3) Live
  sample: runtime MsbPart+0x20 pos == baked to 2 decimals. **Feature feasible + cheap (reuse the walk)
  but UNJUSTIFIED → stays deferred** (design-purity only, zero correctness gain). Real future-proof =
  re-run the offline diff per ERR version bump, NOT a runtime read. Caveat: only 2.2.9.6 on disk →
  proves extraction fidelity for THIS version, not immunity to a future relocating bump.
- **(4) IN-PROCESS DLL probe added + PASSED (2026-06-23, this session).** The external RPM script
  re-implemented the walk; to validate the data the SHIPPING DLL actually collects, added a one-shot
  `[LOOTPOS]` probe (config `diag_loot_pos`, default false) in goblin_collected.cpp: for every loaded
  MSB asset the collected-state walk sees, compare its live `WGMCacheInst{px,pz}` (MsbPart+0x20) to the
  baked `g_entry_positions[row]` (MSB-local, same frame), joined via `g_tile_name_to_row`. Limgrave run:
  **`compared=20 within0.5=20 (100%) maxDXZ=0.00 missingBaked=0`** — byte-exact, in-process, under Wine.
  Position now TRIPLE-validated (offline diff + external RPM + in-process). Probe is gated diag (mirrors
  diag_loot_flags); reusable to re-check per ERR bump. ⚠️ small N at Limgrave (few tracked overworld
  assets); maxDXZ=0 = read is correct (not noise), more samples only widen coverage.
- **⛔ FULL SWITCH-TO-LIVE IS NOT FEASIBLE (user insight 2026-06-23), not just "unjustified."** The live
  geom walk (CSWorldGeomMan) only contains LOADED instances — assets in tiles near the player (the
  `compared=20` at Limgrave = just the nearby loaded loot). Far/unloaded loot has NO live MsbPart → no
  position. Baked covers ALL placements regardless of load state; the map must show everything
  immediately (can't wait for the player to walk every tile to populate positions). So **baked position
  is MANDATORY for full coverage** — a runtime-only position source is structurally incomplete (a moving
  bubble around the player). Same wall as T2 enemy drops (geom = loaded map lots only). The in-process
  probe's role is narrowed accordingly: it VALIDATES the loaded read == baked, it does NOT enable a switch.
  - **GLOBAL all-positions structure = REFUTED (commit e1b502b, `windows_global_item_position_structure_re_findings.md`).**
    Fresh-session scan at Altus: 22 far/unvisited tiles × 2 encodings = 0/44; RTTI = `CS::CSWorldGeomStaticIns`
    (per-block, pos vec4 @+0x250), writer @+0x174E298 = per-region SIMD batch. 5 convergent proofs ⇒
    **per-tile MSB streaming, NO global registry; baked position mandatory + complete.** Loaded-only is a
    hard engine-design limit, not a missing structure → no amount of RE conjures unvisited-tile data.
  - **REMOVE-BAKED / live-only map = NOT VIABLE (reconstruction census 2026-06-23).** Of 8653 baked markers:
    34% asset-backed (geom-walkable), 49% lot-backed (identity needs the asset→lot join), 16% geom-blind
    (Stakes/NPCs/pools/springs/quest = EMEVD/MSB-regions, never in the geom walk). + loaded-only (above).
    So a live map, even 100%-explored, loses ~16% entirely + 49% become anonymous "?" without baked. Baked
    isn't a cache of runtime data — it FUSES MSB-Events+ItemLotParam+EMEVD+curation the runtime doesn't have.
    Progressive-reveal = a DISPLAY/fog-of-war layer ON TOP of baked, never a deletion.
  - **Runtime asset→ItemLotID link EXISTS (commit 4a809cc, `windows_runtime_asset_to_itemlot_re_findings.md`,
    from `windows_runtime_asset_to_itemlot_re_prompt.md`).** lotId is NOT on CSWorldGeomStaticIns/MsbPart;
    it's resident in a `CS::CSGrowableNodePool<FieldInsBase*>` (node `{lotId+flag, FieldIns*}`), FieldIns
    carries `lotId@+0x50` + inline name. ER parses Events.Treasure at tile-load into resident FieldIns
    gimmicks. ⇒ runtime loot identity for LOADED tiles is feasible → enables a COVERAGE SUPPLEMENT / explore-
    cache (catch ERR-added loot with names as you walk in), NOT a bake replacement (FieldIns = loaded-only).
    FOLLOW-ON RE: **STEP 1 SOLVED + STEP 2 structural lead** (commit be1b018, Ghidra find_fieldins{,2..6}.java,
    same findings doc; supersedes the 600c60c RPM "no-link" verdict which missed the embedded pool). Ctors
    self-register each instance into an **FD4Singleton RB-tree** keyed by a 64-bit MSB-derived id. **STEP 1:**
    static slot **er+0x3d7b0c0** (AOBs captured: `48 8B 05` @0x6c5b78 disp 0x036B5541; `48 83 3D..00` @0x72e5d6);
    chain reg=[+0x3d7b0c0], sub=[reg+0x10], map=sub+0x720, RB-tree hdr@+0x8; node +0x20 key(u64)/+0x28 value=instance
    ptr; iter FUN_140b32d00 → **resident field-instance set IS iterable from static base**. Pool RTTI
    `CS::CSGrowableNodePool<CS::FieldInsBase*>` vt 0x2a84ca0, FieldInsBase vt 0x2a25e68. **STEP 2 (2 join paths,
    each needs ONE RPM confirm on chest AEG099_090_9000/lot 1037500100=0x3dd6fec4):** (A cheapest) earlier check
    MISSED the `CSGrowableNodePool<FieldInsBase*>` **embedded in each asset instance @+0x3A8** (node-array@+0x3C0,
    child FieldIns lotId@+0x50, built by FUN_1406c5900) → walked geom +0x3A8 → child → lotId, no global walk;
    (B) iterate the STEP-1 registry, each node+0x28 = field instance → lotId/pos direct (catches ERR-added loot).
    **IN-PROCESS PROBE RESULT (commits 295fb1a..160a01a, diag_fieldins_join, both paths EXHAUSTED):** path A
    REFUTED — embedded asset+0x3A8 pool EMPTY at tile-load (node_arr=0 on all loaded assets incl. target chest;
    pool vt correct) → loot FieldIns NOT parented on asset until later = open-time-only. Path B INCONCLUSIVE —
    documented chain er+0x3d7b0c0→[+0x10]→+0x720→map+0x8 walked as std::map reaches only ~2 nodes (the earlier
    29k was cycle noise w/o a visited-set; sentinel 0xffffffff polluted counts); target lot 0x3dd6fec4 NOT found
    → my chain/node-layout reading is off, NOT confirmable by RPM. ⇒ **escalated to GHIDRA** (new prompt
    `windows_fieldins_registry_layout_and_preopen_re_prompt.md`, commit 160a01a). **MAKE-OR-BREAK Q for the whole
    feature:** does a treasure's lotId-bearing FieldIns exist resident BEFORE the chest is opened, or only after?
    If only-after → explore-cache premium is DEAD (keep baked-only). Ghidra must: answer that + give corrected
    registry chain/node-layout (decompile iterator FUN_140b32d00 + add/rm fns) + treasure FieldIns vtable + lotId
    offset to filter (MapIns vt 0x2a8f650 had +0x50=0xffffffff → +0x50 not universally lotId). **Net:** baked-
    partName-join cache works TODAY for baked loot; ERR-added-loot-with-names is RE-BLOCKED on Ghidra (was wrongly
    called UNBLOCKED). diag flipped off (one-shot done, 967ms walk).
    **GHIDRA ANSWERED (commit da19285, find_fieldins7/8, `windows_fieldins_registry_layout_and_preopen_re_findings.md`):**
    path-B's 2-node walk = TWO offset bugs, now CORRECTED: (1) **missing deref** container=*[sub+0x720] (not sub+0x720);
    self-register map @container+0x18 (std::map _Myhead@+0x08 → head=*[container+0x20], _Mysize=*[container+0x28]);
    (2) **instance = node+0x30** (node+0x28 = per-instance CALLBACK fn ptr FUN_1406c6340 → that's the code-ptr 0x30308e8
    the old probe misread as vtable). Node: +0x00 L/+0x08 P/+0x10 R/+0x19 _Isnil/+0x20 key(u64). Per-frame FUN_140b32d00
    iterates a DIFFERENT map @container+0x00 (active subset) — enumerate via +0x18. **TAXONOMY:** registry = ALL loaded
    FieldInsBase (vt 0x2a25e68) = ChrIns/CSBulletIns/HitInsBase/CSWorldGeomIns/MapIns; **NO dedicated treasure class** →
    lotId carrier = a geom-item (CSWorldGeom*Ins), filter by geom vtable. **RESIDENCY (make-or-break) = sealed chests
    OPEN/SPAWN-time** (path A pool empty + no item class + FieldInsBase has vtable@+0x00 not name → earlier "name@+0x00
    +lotId@+0x50" scan hit was ItemLotParam copy or already-spawned pickup, NOT pre-open treasure) → **explore-cache
    premium limited to PLACED/DROPPED world loot (resident), sealed-chest contents stay baked-only.** Probe re-wired
    with correct offsets (commit 31a5f38); §4 walk = container+0x18 holds CSWorldGeomStaticIns (13-entry subset,
    +0x50=0 not lotId).
    **★ INVESTIGATION CLOSED — premium DEAD for sealed chests (commits ac63c8c..c148fe4, brute [LOTSCAN] memscan):**
    structure-agnostic VirtualQuery scan of ALL committed-private mem for lot 0x3dd6fec4 standing at the UNOPENED
    chest (self stack+module excluded after a 1st-pass false-positive on our own target literal/MBI/".2f}ms" string)
    → exactly TWO hits, BOTH in the ItemLotParam param heap (param-row table + {lotId,index} lookup array), ZERO game
    objects/vtables. ⇒ **sealed-chest loot lotId NOT resident in any usable runtime structure pre-open** (confirmed 4
    ways: path-A pool empty + Ghidra no-item-class/sealed-spawns-at-open + LIVE RTTI [slot 0x3d7b0c0 = CS::RendManImp
    render mgr NOT a field-singleton; its map@+0x18 = 13× CSWorldGeomStaticIns render-geom list, lotId@+0x50=0 = same
    objects our CSWorldGeomMan walk sees] + memscan finds it only in params). [my earlier "FD4Singleton field-instance
    RB-tree" label was WRONG — it's RendManImp's render geom list; merge commit 0a1d024 reconciled both findings]. Placed/
    dropped world loot = geom-item we already walk but carries no resident lotId either → premium has no runtime
    source. ★★ REVERSAL (commit da513922, §7 of findings — link ALIVE for LOADED loot): the "dead" verdict only checked the
    RendManImp registry + asset pool. A full live-mem scan FOUND the real resident link — a 16-byte node
    **{lotId u32@+0, flag@+4, FieldIns*@+8}**; FieldIns→"アイテム" name@+0 + **lotId@+0x50** (self-validating sig:
    *(u32)(FieldIns+0x50)==node.lotId). SAME object carries POSITION: **MapId @ node−0xD8** (0x3c253200=m60_37_50_00),
    **local pos @ node−0xD4** (56.32,238.13,52.68 = baked chest pos) → absolute world pos computable (gridXZ·256+local).
    Owner=**CS::MapIns** region (343 resident); chest pool @MapIns+0x240 EMPTY → lot is an **INLINE record field, not a
    pool entry** → path-A empty-pool was a RED HERRING not proof. Offsets record-specific (1/343 fixed-offset match).
    ⇒ runtime explore-cache for LOADED loot (identity+name+abs-pos) is FEASIBLE. **OPEN CAVEAT = residency:** da513922's
    scan state was UNCONTROLLED. MY [LOTSCAN] at the controlled UNOPENED chest found lotId ONLY in param tables (no node)
    → either node is OPEN/SPAWN-time (sealed premium still limited) OR my scan missed it (LOTSCAN = MEM_PRIVATE-only; if
    the MapIns record is MEM_MAPPED I filtered it out = false negative). RESOLVE = re-scan known-unopened chest incl
    MEM_MAPPED, hunt the SIGNATURE (node{L,flag,P} w/ *(P+0x50)==L) not raw bytes. Then static enumeration anchor (walk
    MapIns set) replaces full-mem scan. Scripts: lot_pos_scan/lot_obj_dump/mapins_enum/verify/final.py.
    **§8 IN-PROCESS BRUTE-SCAN FOLLOW-UP (commits a275bde..bb63808): brute scan CANNOT enumerate the records →
    needs the MapIns anchor (Ghidra).** Target chest 0x3dd6fec4 scan incl MEM_MAPPED at the chest → only in ItemLotParam
    tables, sigNodes=0 (not resident this session: opened/tile-unloaded/open-time, uncontrolled). Generic node scan
    {L,P,*(P+0x50)==L} too slow under Wine (loose prefilter→100M VirtualQuery, never DONE) + collides on 0xffffffff
    filler. アイテム-name scan fast but "item" too common (hits land in arbitrary strings, +0x50=text/sentinels/ptrs,
    no clean per-record lot, chest lot absent). CONTRADICTION: §3 FieldInsBase vtable@+0x00 vs §7 inline name@+0x00 →
    §7's FieldIns* is a DIFFERENT inline-name object (identity unresolved). ⇒ only solid discriminator = node↔FieldIns
    self-validation = needs STRUCTURED walk not byte-hunt. NEXT (Ghidra/CE) = MapIns manager static base + per-record
    offset (record-specific, 1/343) → then re-point dormant diag_lot_memscan to validate in-process. Premium for LOADED
    loot PLAUSIBLE (§7 found 1 real record) but NOT yet enumerable/wired.
    **★ ENUMERATION ANCHOR RESOLVED (commit 188d977, prompt 2328294, `windows_mapins_loot_record_enum_re_findings.md`;
    offline-validated by me on-disk PE):** chain = **WorldMapManImp** (FD4Singleton vt 0x2a8f918, slot er+0x485cbb8) →
    **+0x5d0 WorldBlockMap[]** stride 0x220, count@+0x28 → WorldBlockMap (vt 0x2a8f650) **+0x110 CSGrowableNodePool<MapIns*>**
    (vt 0x2a8f640, cap@+0x120, node_arr@+0x128 stride8) → **MapIns** (vt 0x2a8d6d8). ALL 4 vtable RTTI CONFIRMED from
    on-disk exe; slot 0x485cbb8 = real singleton (796 `cmp qword[rip],0` guard refs in .text). **STEP 0 ship-ready (live-
    proven 343):** bounded vtable-scan for MapIns vt er+0x2a8d6d8 = 343 instances, no chain needed (same pattern as
    scan_all_cursor_instances). **Per-record:** MapIns embeds FieldInsBase pool@+0x240 (EMPTY for chest) → loot is an
    INLINE record found by FieldIns+0x50==lotId signature inside MapIns body (offsets record-specific, NOT fixed MapIns
    offset); node{lotId@+0,flag@+4,FieldIns*@+8}, MapId@node−0xD8, pos@node−0xD4 → abs pos. Class identity RESOLVED:
    item-bearing object = CS::MapIns (MSB map-part), not vtable'd FieldInsBase (fixes §3/§7). **PENDING:** live re-run of
    live_mapins_anchor.py (game closed) to confirm slot+343 yield + placed-world-item residency. **WIRING ATTEMPTED (commits fee828c..b01f551, [MAPINS] walker in goblin_collected.cpp):** ENUMERATION SOLVED
    in-process — vtable-scan er+0x2a8d6d8 = **342 MapIns live**, crash-safe (windowed safe_read, no AV, no hang).
    BUT static chain FAILED live: [er+0x485cbb8] holds an object whose vtable ≠ WorldMapManImp(0x2a8f918) → slot
    candidate WRONG (re-derive real GetInstance slot, or just use vtable-scan). **RECORD NOT FOUND in-process:**
    per-MapIns body scan [+8..+0x2000] for the node sig (lot-range + flag≤1 + *(P+0x50)==L + アイテム name) = **0
    records** at full 342 MapIns, **even after opening a chest** (ruled out timing AND spawn-time-into-body via
    re-armable throttled retry). ⇒ **node NOT embedded in the MapIns body** — da513922's +0x460 was a session heap
    offset, not a stable field; record is in a CHILD ALLOC the MapIns points to (≥1 ptr hop). In-process offset-
    shotgunning NOT converging (8 restarts/1 session). **ESCALATED:** windows_mapins_to_record_reach_re_prompt.md
    (b01f551) → agent decompiles MapIns ctor FUN_14071a0f0 + treasure-registration for the deterministic
    MapIns→record pointer chain. Diag PARKED (flag off, walker code kept). Enumeration is the reusable win.
    LIVE-PROCESS agent prompt written (b96c04f) → **★ AGENT SOLVED THE REACH (commit ed8fa0c,
    windows_mapins_to_record_reach_re_findings.md, live RPM):** node = **MapIns+0x460** {lotId@+0, flag@+4,
    FieldIns*@+8}; **SOLE validator *(u32)(FieldIns+0x50)==lotId** (rejects all false positives — no name/lot-range
    guess); MapId@node-0xD8 (=m+0x388), localPos@node-0xD4 (=m+0x38c); absPos = gridXZ·256+local, gridX=(MapId>>16)&0xff
    gridZ=(MapId>>8)&0xff. My earlier 0-records = the アイテム NAME GATE was too strict (the record's there at +0x460,
    inside my scan range) — DROPPED it. Slot er+0x485cbb8 ABANDONED (use vtable-scan 343). **WIRED in-process (commit
    106d988):** [MAPINS] walker reads +0x460 primary + +0x100..+0x800 hedge window, gate *(FieldIns+0x50)==L, emits
    lotId/MapId/local/ABS. **SCOPE = loaded SPAWNED/OPENED/placed loot only** (live: 1/343 MapIns item-bearing = the
    OPENED chest; sealed chests carry NO resident record → baked-only, §8 stands). For the premium = ERR-added world-
    placed loot + opened/dropped items, runtime identity+abs-pos. **VERIFIED IN-GAME (2026-06-23, [MAPINS] walker):** found real spawned-loot records — e.g. lot=1041360000
    MapId=0x3c292400(m60_41_36) local=(-58.7,0,69.4) ABS=(10437,0,9285), valid FieldIns. Whole chain proven in-process:
    vtable-scan 343 → +0x460 reach (gate *(FieldIns+0x50)==L) → lotId+MapId+absPos. Re-confirmed 2026-06-24. Hedge window
    +0x100..0x800 now DROPPED in code: the "dup" was cross-object bleed (MapIns_A+0x710 == MapIns_B+0x460, SAME node addr,
    not a per-object hedge) → reach is EXACTLY +0x460, emit(0x460) only. chain_ok STILL false (static slot er+0x485cbb8
    wrong → runs vtable-scan fallback, ~1.5s/pass, slow-under-Wine = must move off game thread before ship). NEXT = wire lotId→resolve_loot_item_textid
    (name) + dedup + the explore-cache feature (runtime layer of discovered ERR-added/placed loot on the map). Walker is
    a one-shot heavy scan (~1.5s) on the game thread — move to background probe before shipping.
    **SHIPS NOW = baked-partName-join explore-cache (works today). EXE-SCAN BONUS:** slot 0x3d7b0c0 CONFIRMED
    (both be1b018 AOBs unique→0x3d7b0c0 via on-disk PE scan), RTTI resolved from disk: vt 0x2a86860=CSWorldGeomStaticIns,
    0x2a8f650=WorldBlockMap (not MapIns). Diags diag_fieldins_join/diag_lot_memscan left dormant (gated off). re-diff per ERR bump.
- **STRATEGY LESSON (2026-06-23): brute mem-scan = DISCOVERY only, NEVER production runtime.** LOTSCAN
  "failing" in-process while Ghidra/RPM found the record = NOT a tooling law — it was uncontrolled game
  state + coverage gaps (region-#1 AV, MEM_MAPPED skipped, never reached DONE). The 2 standard strategies
  for reading a runtime structure: (1) **ANCHOR CHAIN from a static base** — singleton via AOB/RVA → fixed
  offsets → structure; deterministic, no scan (= the Ghidra MapIns enumeration: WorldMapManImp[er+0x485cbb8]
  →+0x5d0→+0x110→MapIns, or vtable-scan STEP0=343). (2) **HOOK the creator/consumer** — hook the fn that
  builds/reads the struct, grab the pointer when the engine hands it over; state-correct by construction
  (= how grace suppression works). For the loot record (owned by CS::MapIns): use (1) the MapIns anchor-walk
  + per-MapIns node signature; if residence is spawn-time/transient, use (2) hook the treasure-spawn. Both
  beat the brute scan. The dormant diag_lot_memscan should be RETIRED in favour of the anchor-walk.
- **Icon-doubling (separate from loot-identity):** only categories the GAME draws natively double the
  overlay → Grace (WorldMapWarpPinData, suppressed) + Boss (WorldMapPointParam textId2==5100, suppressed via
  dispMask clear, commit 239ddde/eadc1b4, default ON, in-game verified). Other categories (loot/items) have
  NO native map point in vanilla → overlay-only, nothing to suppress. The dispMask-clear trick generalises to
  ANY native WorldMapPointParam category that ever doubles.
- Related: [[live-param-vs-baked-data]], [[overlay-rendered-markers]]. Write-up:
  docs/loot_identity_probe_result.md, docs/re/loot_ammo_encoding_finding.md.
