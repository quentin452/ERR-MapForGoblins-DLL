# Group-2 (Elevator / Smithing Table) prompt-binding RE — Linux in-DLL session findings

Session 2026-07-02, done ENTIRELY on the Linux box via in-DLL probes (`[PARAMSCAN]`/`[EMEVDSCAN]`,
`src/goblin_param_scan.cpp`, gated on `debug_logging`) + offline python — first end-to-end run of
`docs/memory/tooling/linux-runtime-re-options.md` path 1. Supersedes the param/ObjAct part of
`windows_group2_landscape_re_findings.md`.

## Established facts (live ERR 2.2.9.6 data)

1. **ABP 6250 (Smithing, text 7030) and 5010 ("Descend", text 3301) exist** in ActionButtonParam
   (584 rows, row size 100, textId column +0x34) and are the ONLY carriers of those texts.
2. **ObjActParam does NOT bind them.** Real `actionButtonParamId` column = **+0x28** (195/200 values
   are ABP row ids; 96-byte rows, 208 rows). No ObjAct row references 6250 or 5010 at any candidate
   column (+0x4/+0x10/+0x1C/+0x28). The old "Elevator/Smithing are ObjAct-bound" hypothesis is
   REFUTED.
3. **No param table anywhere stores 6250/5010 as a meaningful field** (whole-repository needle scan;
   remaining hits are value coincidences — EquipParamWeapon +0x9 unaligned family pattern,
   NpcParam +0x1E8 unrelated field, etc.).
4. **EMEVD sweep (517 files) — the needles appear in exactly 3 places:**
   - `common.emevd` event **1030**: 19 leading `3[37]` waits on GENERIC ABP ids
     {1000, 2000, 2010, 2020, 3000, 3010, 3020, 3021, 3022, 4000, 4001, 4010, 4020, 5000, 5010,
     5011, 6000, 0x24F47364, 0x24F47365(ERR custom)} then per-ABP `1000[101]` branch +
     `2003[68]`(machine idx, -1.0f, 0) blocks. **1030 = the global asset-prompt DISPATCHER** — no
     entities anywhere; its single init passes arg 0. The asset↔ABP binding lives ENGINE-side
     (asset params/HKS/TAE), NOT in EMEVD and NOT in params (see 3). ⇒ **no per-lift EMEVD harvest
     exists.**
   - `m11_10` (Roundtable): **115 inits of template 1042582000 (ABP, entity)** — the hub's
     generic prompt template (body: `3[16]` IfActionButton param-substituted → `2003[66]` set
     flag). ONE init carries 6250: args (6250, entity 1042580062) = Hewg's smithing table. The
     only 6250 in the entire event tree ⇒ world "Smithing Table" spots (MapGenie category) are
     NOT ABP-6250 prompts; their in-game mechanism must be identified from ground truth (what
     text/prompt does a world smithing table actually show in ERR?).
   - `m12_02` (Siofra) events **12022820 / 12022822**: one-off map events using 5010 inline
     (`2004[8]`) — these DO reference concrete lift entities; body dump pending (round 5) → grep
     entity ids in MSB → the real lift AEG model id.

## RESOLUTION (2026-07-02, [ABPTEXT] slot-32 breakthrough)

The prompt-text FMG = **physical GetMessage slot 32** (3301="Descend", 7030="Use smithing table").
Full ABP text dump (`logs/abptext_slot32.txt`) settles everything:

- **ABP 5000/5010/5011 = "Climb"/"Descend" = LADDERS**, not elevators — the original recon anchor
  was wrong from the start. Event 1030's generic-ABP list (1000 runes, 2000 summon sign, 3000 read
  message, 4000 pick up item, 5000/5010 ladders, 6000 talk…) = the engine's built-in interaction
  prompts, all engine-bound, none map-category material.
- **Real lever-driven lifts = "Pull lever"/"Push lever" = ABP 8200–8501 (+ERR 208310/208405)** —
  and THOSE are ObjAct-bound: **~55 ObjActParam rows** carry them at +0x28 (ids 27002…1464026,
  offline join of paramdump_ObjActParam × abptext_slot32).
- **The asset join is the MSB ObjAct EVENT section**: MSB events of type ObjAct bind
  {asset entity, objActParamId}. ⇒ **Elevator category = pure disk parse**:
  `MSB ObjAct event → objActParamId whose ObjActParam.actionButtonParamId(+0x28) has a
  lever/lift text → asset → position`. Mod-agnostic, no bake, same pipeline class as Portal.
  Refinement knob if levers over-capture (some open gates, not lifts): ObjAct anim-id fields
  (14020 vs 7110/8000 groups visible in the dump) or asset model.
- **Grand lifts (Dectus/Rold)** = ABP 5762320/5762330 "Enter Field Area" — already covered by the
  WorldGrandLift category.
- **Smithing Table: SOLVED — model filter `AEG099_308`.** [ASSETRADAR] at the Church of Elleh
  table put AEG099_308 (entity 1042361700, the Whetstone-Knife treasure event) + companion
  AEG099_309 at ≤2 m; [ASSETCOUNT] world census: **AEG099_308 = 3 placements**
  (m33_00_00_00 entity 33001035, m60_38_51 no-entity, m60_42_36 Elleh) — exactly the visible
  world smithing tables (Roundtable is a hub, off-map). Category = MSB assets with model
  AEG099_308, same disk pass class as everything else. (Prompt ABP 6250 stays engine-bound —
  irrelevant for the map category now.)
- Siofra "hits" (12022820/22) decoded: `2004[8]` = SetSpEffect(character, 5010) — SpEffect id
  homonym on two enemies, red herring.

## Tooling notes (reusable)

- `[PARAMSCAN]`: needle scan over all param tables + raw dumps (`logs/paramdump_<table>.txt`).
  ParamRowInfo.param_end_offset is NOT the row end (points at the name blob) — row length = delta
  to the next row's param_offset.
- `[EMEVDSCAN]`: needle scan over instruction args of every `event/*.emevd.dcx` (in-process
  oo2core via GetModuleHandle, same DCX path as loot_disk), template-init harvest for BOTH
  2000[0] InitializeEvent (eventId@+4) and 2000[6] InitializeCommonEvent (eventId@+0), and
  full-body dumps (`logs/emevddump_<eventId>.txt`) for offline EMEDF decoding.
- Needles/templates/dump-lists are hardcoded consts at the top of `goblin_param_scan.cpp` — edit +
  rebuild (~1 min) per hunt round; each round costs one game restart.
