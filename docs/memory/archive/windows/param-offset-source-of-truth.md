---
name: param-offset-source-of-truth
description: "Offset source-of-truth for the no-bake refactor — live bytes are truth, the committed Paramdex DRIFTS; tools/check_param_offsets.py is the advisory guard"
metadata: 
  node_type: memory
  type: project
---

**The "industrial offset-free" refactor (Phase 3 / [[nobake-endgame-roadmap]]) was investigated
2026-06-26 (commit e3333d6, docs/re/offset_source_of_truth_audit.md). Outcome: NO offset code change
needed — the item LOADING is already generic (live param chains, not per-item tables), so the only
hand-pinned data is ~30 param/disk field offsets, and those are already LIVE-validated (the real
source of truth).**

Key facts for any future offset work:
- **The game has NO runtime paramdef.** Field offsets are compiled into eldenring.exe's code
  (`mov al,[row+0x3e]`). You cannot parse a paramdef from the game — it's compiled-in.
- **SoulsFormats Paramdex IS committed** at `tools/paramdefs/*.xml` (~200 defs incl. EquipParamGoods,
  ItemLotParam, NpcParam, AssetGeometryParam=AssetEnvironmentGeometryParam, GestureParam,
  BonfireWarpParam). `<ghidra_scripts>\paramoff.py` + repo `tools/check_param_offsets.py` compute field
  offsets from it. Bitfield packing rule: pack by STORAGE SIZE not type name (`u8 x:1`+`dummy8 y:7` =
  ONE byte) — getting this wrong adds a spurious byte per mixed run.
- **The committed Paramdex DRIFTS from ERR's regulation** (app 2.6.2.0): of 11 checked, 7 agree, 4 don't
  — sortGroupId (pd 0x73 vs code 0x72), pickUpItemLotParamId (pd 0xb9 vs 0xb8), textId1 (pd 0x31 vs
  0x30), and **isEnableRepick (pd bit6 vs LIVE-validated bit5)** = the historical 16k-leak field (the
  original bug literally used the Paramdex's bit6). So **Paramdex ≠ authoritative; LIVE param bytes are
  the truth** (pin via raw bytes + POSITIVE/NEGATIVE, see [[re-offset-validation]]).
- **exe-AOB idea** (read the offset from the access instruction's displacement — authoritative + self-
  correcting) is mechanically trivial but EXPENSIVE per field: a displacement scan (find_goods_access.java)
  surfaces generic serializers, not the field read site; you must anchor on the param (string →
  SoloParamRepository → getter) per field. Not worth it for the bulk.
- **The guard**: `tools/check_param_offsets.py` is an ADVISORY cross-check (soft — flags the 4 known
  drifts, HARD-fails only on a NEW/unexpected divergence). Run it after touching any param offset. A
  fully-automated HARD guard would need a live-RPM SoloParamRepository row reader (not yet scripted;
  asset_lot_probe.py walks geom, not params).

Strategic: the no-bake item pipeline is already content-agnostic (→ [[overlay-item-search-bar]] notes
randomizer foundation). Option-3 (delete the bake) is the remaining lever for randomizer compat, NOT
the offset refactor.

**▶ RESUME POINT — READ the offsets DIRECTLY from the game at runtime (<user>'s clarified goal,
2026-06-26). THE NUANCE: do NOT extract-and-hardcode an offset constant; have the DLL READ the offset
from the source of truth (the game's own compiled code) at init → zero hardcoded offset constants,
self-correcting across patches.** The Paramdex drifts and pinning row bytes is "extract" (rejected as
the goal). The single source of truth = the game's code: `mov al,[row+0x3e]` carries 0x3e as the
instruction's DISPLACEMENT. So:
- **Runtime mechanism**: AOB-resolve the field's access instruction in live eldenring.exe → parse its
  ModRM/disp → the displacement IS the offset, read live. Store in a resolved var (NOT a constant);
  every param read uses it. (x86-64 disp8/disp32 is at a fixed position after ModRM/SIB — trivial to parse.)
- **The cost we measured**: locating the access SITE per field is the work — a blind displacement scan
  (find_goods_access.java) is too noisy (0x3e/0x72 ubiquitous; surfaces generic serializers). Author a
  STABLE AOB per field by ANCHORING on the param: the "EQUIP_PARAM_GOODS_ST" string → SoloParamRepository
  param index → the getter/caller that fetches the row and reads the field. Do this ONCE per field to
  author the AOB; thereafter the AOB resolves the offset live each run.
- **Alternative "read directly"**: instead of the offset, AOB + CALL the game's own accessor function
  for the field (the offset stays inside the game's code — we never see it). Also per-field RE to find
  the getter, but no displacement parsing.
- **RPM/Python role** = PROTOTYPE this (resolve a candidate site, read the displacement live, sanity-check
  vs a known row), and help author the AOBs — NOT to pin-and-hardcode. RPM boilerplate in
  `<ghidra_scripts>\asset_lot_probe.py`; the DLL's existing live-param chain is in `src/from/params.hpp`
  (`get_param<T>` → SoloParamRepository) = the recipe for reaching a row to validate against.
- Targets to resolve live: goodsType(0x3e ok), sortGroupId(0x72 vs pd 0x73), AEG pickUpItemLotParamId
  (0xb8 vs pd 0xb9) + isEnableRepick (bit5 vs pd bit6, the 16k-leak field), BonfireWarp textId1(0x30 vs pd 0x31).
Game must be running. End state: the DLL resolves these offsets from the exe at init; `check_param_offsets.py`
becomes a build-time sanity cross-check against the Paramdex, not the source.

**▶▶ PROGRESS 2026-06-26 — PROTOTYPE BUILT + PROVEN LIVE (`<ghidra_scripts>\offset_resolver.py`; audit
doc has the full writeup).** The runtime mechanism is DONE and validated against the running game:
- **Engine is trivial** — extracting the displacement is a sibling of the DLL's existing `modutils`
  `relative_offsets` (read N bytes at a fixed pattern pos); just RETURN the value vs use it as a ptr delta.
  No runtime disassembler needed: the authored AOB pins the disp position. (`decode_disp()` is only for
  AUTHORING — parses prefixes/REX/0F/ModRM/SIB/disp8/disp32; self-test 6/6.)
- **Live param oracle works** — ported `get_param` to RPM (SOLO_PARAM_LIST AOB → EquipParamGoods 7126 rows
  → any field byte; e.g. goods 8000 goodsType=1). This is the ground-truth validator.
- **author→resolve round-trip PROVEN** end-to-end on a real exe instruction (unique 24-byte AOB →
  runtime-resolved disp = live value). Uniqueness gate fires correctly.
- **The ONLY remaining cost = per-field READ-SITE capture.** Blind 0x3e scan = noise (find_goods_access:
  4 hits all WRITES/ctors). Fast loop = CE **"find what accesses"** on a LIVE row addr; the script PRINTS
  the exact `rowAddr+fieldOff` target (e.g. goodsType watch printed live). Then
  `py offset_resolver.py author 0x<RIP>` emits a uniqueness-checked AOB + disp_pos for re_signatures.hpp.
  CLI: `author` / `resolve` / `groundtruth`.
- **C++ ENGINE IS IN (compiles+links clean):** `modutils::resolve_field_offset({aob, disp_pos, disp_size})`
  (src/modutils.cpp/.hpp) — AOB-scans (refuses unless UNIQUE), reads disp8/disp32 at disp_pos, returns the
  live offset. Displacement sibling of `relative_offsets`. Not yet wired to any field (waiting on the AOB).
- **✅✅ goodsType RUNTIME-VALIDATED IN-GAME (2026-06-26, deployed build, session 19:58).** Log:
  `[SIG] PASS GOODS_TYPE_ACCESS` + `25 unique/0 ambiguous/0 missing — all clean`;
  `[FIELDOFF] EquipParamGoods.goodsType = +0x3e (live from exe)` (no fallback); census UNCHANGED
  (collectibles 1461/0-unclassified, disk 3642 lots). First field where the offset is READ from the
  exe's code at init — zero hardcoded 0x3e in the read path. Mechanism PROVEN real end-to-end. Not committed.
- **✅ goodsType WIRED END-TO-END (2026-06-26, builds+links clean).** CE
  find-what-accesses captured the read site: `cmp byte ptr [rcx+0x3e],0Dh` at eldenring.exe+0x25A848, RCX=
  goods row, RDX=EquipParamGoods table (param-anchor confirmed). Authored AOB `48 85 C9 74 ?? 80 79 ?? 0D`
  (`test rcx,rcx; je ??; cmp [rcx+??],0D`) — UNIQUE, jump-rel8 + disp wildcarded so only stable bytes
  remain; disp_pos=7 size=1 → resolves 0x3e live. Added `GOODS_TYPE_ACCESS` to re_signatures.hpp (+ SIG
  table) and rewired `goods_type_live` (goblin_inject.cpp) via `goods_type_offset()` →
  `modutils::resolve_field_offset`; the hardcoded 0x3e is now only a LOGGED fallback. Runtime check:
  log should show `[FIELDOFF] EquipParamGoods.goodsType = +0x3e (live from exe)` + `[SIG] PASS
  GOODS_TYPE_ACCESS`, and item census/classification UNCHANGED.
- **🔧 EMBEDDED FIND-WHAT-ACCESSES BUILT (2026-06-26, compiles+links; src/goblin_field_probe.cpp/.hpp).**
  Static read-site discovery is a DEAD END for sortGroupId/AEG (Ghidra: 72 `[reg+0x72]` reads + 6507
  `[reg+0xb8]` reads, NONE in param-anchored fns — the game decouples row-fetch from field-read; the param
  strings have no code xref). And CE is drowned by 6 param-reading mods (ptr-scan module list: reforged,
  ertransmogrify, ermaterialparamexposer, Scripts-Data-Exposer-FS, er_console_mod, MapForGoblins). SOLUTION:
  a DLL probe = VEH + hardware breakpoint (DR0) on a LIVE param row+offset; the handler logs `[FWA]` the
  FIRST accessing instruction whose RIP is inside eldenring.exe (every mod read auto-filtered). Config:
  `probe_field_access=true` + `probe_field_spec=ParamName:rowId:offset[:len[:rw]]` (DLL resolves the live
  row via get_param → session-independent). NOTE: data-bp RIP = the NEXT instr (trap); the log dumps a byte
  window so offset_resolver.py back-decodes the accessing instr. Dev-only, EAC-bypassed only (writes DR regs).
  Build gotcha hit: PowerShell splits `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` at the dot → MUST quote the -D args.
- **✅✅✅ sortGroupId RUNTIME-VALIDATED IN-GAME (2026-06-26, session 21:18).** `[SIG] PASS
  GOODS_SORT_GROUP_ACCESS` (26 unique/0/0); `[FIELDOFF] sortGroupId = +0x72 (live)` + goodsType=+0x3e,
  no fallback; census IDENTICAL (collectibles 1461/0-unclassified, disk 3642 lots). TWO fields now read
  their offset from the exe. The embedded find-what-accesses TOOL is proven (cracked the field that beat CE).
  The embedded find-what-accesses caught the game's read filtered from 6 mods: `[FWA] eldenring.exe READ
  by rip=er+0x86eeef`, accessing instr `movzx ebx,byte [rax+0x72]` at **er+0x86eeeb** (RAX=goods row
  10105 verified). This was ONE of the 72 static lookalikes — the HW-breakpoint pinned THE right one,
  validating the whole tool. Authored AOB `0F B6 58 ?? EB ?? B3 FF 85 FF` (jump rel8s + disp wildcarded;
  unique; disp_pos=3 size=1 → 0x72). Added GOODS_SORT_GROUP_ACCESS to re_signatures.hpp (+SIG table),
  rewired `goods_sort_group` via `goods_sort_group_offset()`. **Gotchas learned:** (1) target must be a
  GOODS the game actually reads — the Crimson flask (1000) is special-cased/never read by the menu; a
  normal held material (Smithing Stone [6]=10105) works. (2) trigger = force an inventory REBUILD (reload
  save / acquire / re-sort), a mere menu-open reads from cache. (3) data-bp RIP = NEXT instr; back-decode
  the window (the instr at rip-4 here). Items name→id via data/items_database.json (cat=1=goods,3=protector).
- **✅ AEG pickUpItemLotParamId CAPTURED + WIRED (2026-06-26, builds clean; awaiting in-game test).** Probe
  on `AssetEnvironmentGeometryParam:99821:0xb8` (Runic Trace AEG099_821, the most ubiquitous ERR collectible
  — 1397 in-world), triggered by LOOTING one. `[FWA]` → `mov eax,[rax+0xb8]` at er+0x6c4c11 (RAX=AEG row,
  +0xb8=998210 verified). AOB `8B 80 ?? ?? ?? ?? 85 C0 79 ?? 48 8B 43 08` (disp32 + jns rel8 wildcarded;
  unique; disp_pos=2 size=4 → 0xb8). Wired `aeg_pickup_lot` via resolve_field_offset (s_off; pinned 0xb8 =
  fallback). AEG row id = 99000+modelNum; collectible model→item in data/aeg099_item_mapping.json.
- **✅✅✅ pickUpItemLotParamId RUNTIME-VALIDATED + COMMITTED (commit 1ca8e79, session 21:32):** `[SIG] PASS
  AEG_PICKUP_LOT_ACCESS` (27 unique/0/0); `[FIELDOFF] pickUpItemLotParamId = +0xb8 (live)`, no fallback.
  THREE fields now read their offset from the exe.
- **⚠️ isEnableRepick — bit nuance (2026-06-26):** probe on `99821:0x3c:1:r`, looting a Runic Trace, caught
  `test byte ptr [rcx+0x3c], 0x01` at er+0x6a867f (RCX=AEG row). This tests **bit 0** (a sibling bool), NOT
  isEnableRepick's **bit 5** (0x20). So the capture CONFIRMS byte 0x3c live but does NOT give the bit. Our
  bit 5 stays the empirical 16k-leak-fix value. The one-shot probe disarms on the first eldenring.exe hit →
  to catch the SPECIFIC `test [reg+0x3c],0x20` (bit-5) site we'd need a MULTI-HIT probe (log N distinct RIPs,
  not disarm after 1) then pick the mask-0x20 one — that would resolve offset 0x3c AND the bit live (fully
  close the 16k-leak field). DECISION PENDING (<user>): (A) enhance probe to multi-hit + catch bit-5,
  (B) leave isEnableRepick pinned (byte cross-validated, bit 5 empirical), (C) wire byte-only.
- **✅ isEnableRepick SOLVED — offset AND bit both live (2026-06-26, builds clean; awaiting in-game test).**
  <user>'s insight: the bit-0 read was a sibling bool, not the repick read. MULTI-HIT probe (enhanced
  goblin_field_probe to log N distinct sites, dedup by RIP, disarm after 16) surfaced 7 reads of byte 0x3c,
  one per bitfield bit (bit0/3/6/7…). hit #6 = `movzx eax,[rax+0x3c]; shr eax,5; and eax,1` at er+0x6c4c59
  = THE bit-5 read = isEnableRepick → LIVE-RE-CONFIRMS the 16k-leak fix (bit 5, not Paramdex bit 6). AOB
  `0F B6 40 ?? C1 E8 05 83 E0 01 C3 C3`: disp8 at pos 3 = offset (0x3c, wildcarded → resolves live), `shr`
  imm at pos 6 = bit (5). aeg_is_gather reads BOTH via two resolve_field_offset calls → `(b[off]>>bit)&1`,
  zero magic numbers. The `05` is kept in the AOB to pin the bit-5 site (bit-6 sibling is else byte-identical).
  Disp-wildcarded-only matched 3 structs → needed the `C3 C3` accessor epilogue for uniqueness. Validation:
  collectibles must stay ~1461 (NOT balloon to 16k = the leak). KEY LESSON: a bitfield bit is semantic
  (which bit=which field) — derivable only by anchoring on the game's bit-N extraction; the multi-hit probe
  is what makes that capturable.
- **FOUR fields now read offset (and isEnableRepick's bit) from the exe: goodsType, sortGroupId,
  pickUpItemLotParamId, isEnableRepick.** Tooling: goblin_field_probe (multi-hit), offset_resolver.py.
- **✅ BonfireWarp.textId1 CAPTURED + WIRED via CONSENSUS (2026-06-26, builds clean; awaiting in-game test).**
  Probe `BonfireWarpParam:61423601:0x30:4:r` ("The First Step" grace, textId1=610019), triggered by the
  warp menu. textId1 is read via a GENERIC switch-dispatcher text getter (`mov reg,[base+0x30]` / +0x3c /
  +0x48 cases) REUSED across several param types → the AOB `41 8B ?? ?? EB 2B 41 8B ?? 3C EB 25 41 8B ?? 48
  EB 1F` matches 4 sites, ALL disp 0x30. Added a CONSENSUS mode to modutils::resolve_field_offset (accept
  N matches iff all agree on the disp; FieldOffsetArgs.consensus=true) — the clean answer for shared getters.
  NOT in the SIG table (intentionally multi). textId1 was read via the typed BONFIRE_WARP_PARAM_ST struct
  (static_assert offsetof==0x30); now capture_live_graces reads it through the resolved offset (textid1_of
  lambda). Validation: graces still appear with correct names. To find a grace row offline: dump live (handle
  REOPEN after import — the driver closes it) + label textId1 via data/PlaceName_engus.json.
- **getItemFlagId (+0x80) — INVESTIGATED, kept PINNED (commit after ccdcbbc).** <user> asked to extend the
  runtime-read to the collected-flag path (resolve_loot_flag reads ItemLotParam.getItemFlagId@0x80 / 01@0x60
  live + IsEventFlag — the pipeline is already multi-mod, only the offset is hardcoded). Probe
  `ItemLotParam_map:998210:0x80:4:r`, triggered by looting a Runic Trace → caught `mov eax,[rax+0x80];
  cmp eax,-1; cmove; ret` at er+0xd3be3f. BUT this generic tiny getter is BYTE-IDENTICAL to its 0x84-field
  sibling (er+0xd3bde0) — disp-wildcarded AOB → MULTI (2 sites, 0x80 vs 0x84), and consensus fails (they
  disagree). No unique anchor possible (the offset is the only difference = circular). Reverted; left pinned
  0x80 (live-confirmed + check_param_offsets guard). **LESSON: a field read only via a trivial shared getter
  with an identical-shape sibling has NO runtime-resolvable anchor** — distinct from textId1 (switch →
  consensus) and the 5 done fields (distinctive read). Untried fallbacks if ever needed: spawn-check inline
  read (one-time lot + reload trigger) or two-hop caller-anchored resolve.
- **🏁 ALL FIVE TARGET FIELDS now read their offset from the exe at init: goodsType, sortGroupId,
  pickUpItemLotParamId, isEnableRepick(+bit), textId1(consensus).** Engine: modutils::resolve_field_offset
  (unique + consensus modes). Tool: goblin_field_probe (multi-hit, eldenring.exe-filtered). check_param_offsets.py
  = the build-time Paramdex cross-check, as planned. Commits 72c765d/13a1107/1ca8e79/90ee351 + textId1 (pending).
- **(earlier NEXT):** <user> deploys + tests sortGroupId in-game (expect `[FIELDOFF] sortGroupId = +0x72 (live)` +
  `[SIG] PASS GOODS_SORT_GROUP_ACCESS` + census unchanged). probe_field_access set back to false. Then AEG
  `pickUpItemLotParamId`: `probe_field_spec=AssetEnvironmentGeometryParam:<row>:0xb8:4:r`, trigger by
  looting the matching collectible (need an AEG row id the player can reach). Then BonfireWarp textId1.
  Consider committing field_probe + sortGroupId (<user> asked about committing the tool).
- **(historical NEXT):** repeat the goodsType loop for sortGroupId(+0x72),
  AEG pickUpItemLotParamId(+0xb8)/isEnableRepick(+0x3c bit5), BonfireWarp textId1(+0x30). For sortGroupId,
  CE find-what-accesses `0x1f648e2c1c2`-style (row+0x72) — likely the menu sort comparator. isEnableRepick
  is a BIT not a byte offset → resolve_field_offset gives the byte (0x3c); the bit (5) still needs its own
  capture or stays pinned (note the bit in the AOB comment).
- **Verdict for <user>:** mechanism is real and self-correcting for the COMMON drift case (offset shifts
  but instruction shape stays → AOB still matches, auto-resolves new offset, zero human action) — strictly
  better than a hardcoded constant. But it costs ~6 one-time CE captures + AOB maintenance, and the shipped
  `check_param_offsets.py` Paramdex guard already catches gross drift at build time. Decision pending: scale
  to all 6 targets vs. keep the build guard.
