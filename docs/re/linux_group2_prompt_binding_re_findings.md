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

## Where this leaves the map category

- **Elevator:** per-lift EMEVD inits don't exist, so the category must be built from **MSB assets
  filtered by the lift model(s)** (mod-agnostic, same class as other model-keyed passes). The
  Siofra one-off entities are the key to identifying the model. Open: whether ONE model covers the
  ~40 real lifts (recon's AEG099_630 = 235 placements was too broad — likely the wrong model).
- **Smithing Table:** ground truth needed from the user before more scanning (ERR-specific
  mechanism; ABP 6250 is Roundtable-only).

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
