# RE FINDINGS (Windows, 2026-07-03) — enemy MODEL → display NAME, runtime source

Answers `windows_enemy_name_runtime_source_re_prompt.md`. All evidence is offline (SoulsFormats
pipeline on the UXM-unpacked vanilla install + the ERR 2.2.9.6 overlay); no live game needed.
Probe scripts: `tools/probe_enemy_name_sources.py` (FMG dumps + param field enumeration),
`tools/scan_emevd_nameids.py` (EMEVD byte-scan).

## VERDICT — POSITIVE: a mod-agnostic runtime mechanism EXISTS (no bundled table)

The Linux session's "all avenues dead" conclusion was wrong on two counts, both id-convention
misses. The full resolution chain, in priority order, every tier reading ONLY the active
install's own regulation + msg files:

1. **`NpcParam.nameId` → NpcName** (already implemented in `goblin_enemy_names.cpp`). Named
   entities: invaders, hostile NPCs, some minibosses. Both installs.
2. **Bestiary probe — `TutorialTitle` id = `model*1000 + variant*100 + {4,10}`** (runtime:
   `lookup_text_utf8(id + 900000000)`, band already routed in `goblin_messages.cpp:65` → physical
   slots {475,375,207}), then strip the `^\d+[a-z]?\.\s*` codex prefix ("116. Tree Sentinel" →
   "Tree Sentinel"). On ERR this names **every generic enemy** — sheep, springhares, soldiers,
   trolls included (~298 models; see below). On vanilla the probe cleanly misses → tier 3. This is
   a *mechanism*, not ERR data: any mod shipping a codex resolves; mods without fall through.
3. **Boss-band probe — `NpcName` id = `900000000 + model*1000 + suffix(0..999)`**. FromSoft's own
   authoring convention for boss-bar names (they are EMEVD-authored, see §2 below). Enumerate the
   1000-wide band via GetMessage once per model and cache; first non-empty hit wins. Covers
   vanilla field bosses / minibosses whose `NpcParam.nameId` is 0 — **including c3251 Tree
   Sentinel at 903251600**.
4. Fallback: no name (vanilla's own UI shows none for generics — nameless = vanilla-correct), or
   the raw `cXXXX` id for diag.

Verified offline on all 5 prompt models, both installs:

| model | vanilla | ERR |
|---|---|---|
| c3251 (field boss) | tier 3: NpcName 903251600 "Tree Sentinel" | tier 2: "116. Tree Sentinel" (also tier 3) |
| c4311 (humanoid) | tier 3: NpcName 904311000 "Soldier of Godrick" (boss variant only) | tier 2: "79. Godrick Soldier" |
| c4600 (troll) | — (generic; nameless in vanilla data) | tier 2: "172. Troll" (+172a/172b variants) |
| c6060 (sheep) | — | tier 2: "8. Sheep" (+8a–8d, 9x variants) |
| c6100 (hare) | — | tier 2: "1. Springhare" |

## Why the Linux probes missed it

- The NpcName id-formula set (`model, model*10 … model*10000, npc, npc/10 …`) never tried
  **`900000000 + model*1000`**. The 9×10⁸ band holds ~257 vanilla boss entries (903251600–602 =
  "Tree Sentinel", 903250600 = "Draconic Tree Sentinel", 903181000 = "Red Wolf of Radagon", …).
- `TutorialParam` wasn't among the params checked. On ERR it is a model-keyed bestiary: rows
  `model*1000 + variant*100 + n` with `textId` → TutorialTitle/TutorialBody. The pipeline's
  `data/enemy_tutorial_mapping.json` is literally `{cNNNN: NNNN*1000+4}` **extracted from this
  param at bake time** (`tools/extract_tutorial_codex.py`) — the runtime probe supersedes that
  bake per the prime directive.

## §1 param enumeration result (exhaustive)

All 194 regulation params were paramdef-applied and every field matching
`/(name|text|msg|caption|title)(Id)?$/i` probed at model-derived row ids (both installs). The
ONLY enemy-name-bearing paths are `NpcParam.nameId` (tier 1) and ERR's `TutorialParam.textId`
(tier 2). Everything else is unrelated text (shop titles, menu captions, map names) or all-zero:
`GameAreaParam.foundBossTextId/notFindBossTextId` are 0 for every model-derived row,
`RoleParam.roleNameId`, `MenuPropertyLayoutParam`, `FeTextEffectParam`, etc. — dead. Full log
preserved in the probe script output (rerun to regenerate).

- Vanilla NpcParam: 7039 rows, 715 with `nameId != 0` (695 resolve in NpcName). ERR: 6868 rows,
  930 nonzero (912 resolve). Every 3251xxxx/6060xxxx/6100xxxx row = 0 in both, confirming the
  Linux live reads.

## §2 the field-boss banner — SOLVED (and it kills §3)

- The runtime `fmg=32` the Linux GetMessage hook saw is **physical slot 32 = ActionButtonText**
  (`goblin_messages.cpp` `kAction[] = {32}`), not event/menu text. ERR authored the WHOLE phrase
  at **ActionButtonText id 5763010 = "Enter Field Boss: Tree Sentinel"** (action-region prompt)
  plus a matching **NpcName id 5763010 = "Field Boss: Tree Sentinel"** (the bar/banner name).
  There is no `{name}` template argument to trace — the id is authored per boss.
- Vanilla has **no id 5763010 anywhere**. Vanilla boss names are authored per-map in EMEVD:
  `HandleBossHealthBar(entity, slot, nameId)` with nameId in the 9×10⁸ NpcName band. Byte-scan
  confirms: `903251600` in `m60_42_36_00.emevd` (Gatefront — the Tree Sentinel the prompt names)
  and `m60_41_51_00.emevd` (x2), `903250600` in `m60_45_52_00/10.emevd` (Draconic), `904311000`
  in `m18_00_00_00.emevd` (Stranded Graveyard, Soldier of Godrick) — same linkage intact in the
  ERR overlay's emevds.
- **§3 (engine ChrIns→name resolver) is therefore moot, not pursued**: display names are
  hand-authored ids in EMEVD/params, never derived from the model by engine code. There is no
  `GetChrName(ChrIns*)` to find beyond the `nameId` read we already replicate; a mob with
  `nameId=0` renders a nameless bar because the engine genuinely has no name for it.

## §4 NpcName dump — generic-name negative CONFIRMED

Full engus dump (base + dlc01 + dlc02 merged): vanilla **480 entries** — ~223 named characters in
the low band (121600 "Blaidd the Half-Wolf" style) + ~257 bosses in the 9×10⁸ band. ERR: 665
(same layout + ERR's own 57xxxxx "Field Boss: X" band, 89 entries, + custom bosses). **No entry
for any generic field enemy in either install** — wiki names like "Godrick Soldier" for the
non-boss c4311 variants are community-authored, exactly as suspected. So on pure vanilla,
generics stay nameless (which matches vanilla's own HUD) unless a curated table is ever wanted —
see the prompt's §Fallback; NOT recommended, it violates the prime directive for zero
vanilla-visible gain.

## Implementation notes (for `feat/enemy-bar-names`)

- Inputs already live: `ChrIns+0x64` model, `+0x60` npcParamId; variant = `(npcId % 10000)/1000`.
- Tier 2 order: `model*1000 + variant*100 + {10,4}`, then variant-0 `{10,4}`. Caveat: variant
  digits don't always match the codex sub-entry (ERR NpcParam 46004238 "Rennala's Troll" vs codex
  4600104), so the variant-0 fallback is required; worst case names the base species — acceptable.
  Titles are clean text (no HTML; the `NNN[a-z].` prefix strip is the same regex the old
  `verify_enemy_name_runtime.py` used).
- Tier 3: 1000 GetMessage calls per new model, once, cached (GetMessage is bounds-checked and
  cheap; PostureBarMod-style per-frame budget unaffected). Multiple in-band hits are same-boss
  variants (903251600..602 identical) — take the first non-empty.
- Localization is free: GetMessage serves the active language's FMGs.
- Prime-directive acceptance: every tier reads the ACTIVE install's regulation/msg only. A
  different mod with different params/textures gets its own codex names (tier 2), its own EMEVD
  boss names (tier 3), or vanilla-correct namelessness — no ERR-frozen data anywhere.
