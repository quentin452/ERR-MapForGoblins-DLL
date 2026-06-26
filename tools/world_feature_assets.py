#!/usr/bin/env python3
"""Declarative source of truth: AEG asset-model World features → marker category.

This is the EDITORIAL model→feature map the runtime disk pass uses to off-bake
asset-model World features (Stakes, Imp Statues, Hero's Tomb, …) without a
per-feature C++ function. tools/generate_world_feature_models.py transcodes it
to src/generated/goblin_world_feature_models.{hpp,cpp}; the runtime then runs ONE
generic pass (build_disk_world_feature_markers) over the mod's real MSBs, looks
each placed asset's aegRow up here, and emits a marker. Adding a new asset-model
World feature = ONE row below, regen, done — zero new code.

Why a hand-maintained table (not derived live): unlike collectibles/treasure, these
assets carry NO live game param that gives the MFG category (no pickUpItemLotParamId,
no item lot) — the category is purely an MFG editorial choice. So the model→category
binding has to live SOMEWHERE; one committed table covers every asset-model feature.

SCOPE: asset-model features ONLY. Region/param/EMEVD/enemy features (Spirit Springs,
Summoning Pools, Quest/Hostile NPCs, Paintings) keep their own bespoke passes.

Row fields:
  aeg_row          AssetEnvironmentGeometryParam row id = AEG{A}_{B} → A*1000 + B.
                   (e.g. AEG099_060 → 99060). This is what the MSB part name parses to.
  model            The MSB ModelName, for readability/comments only.
  category         C++ goblin::generated::Category enum NAME (must exist in the enum).
  text_id          WorldMapPointParam.textId1 for the tooltip label. Tutorial-text ids
                   (9003xxxxx), FMG item names (5000xxxxx), or ActionButtonText via the
                   +800,000,000 offset (goblin_messages copies those at runtime).
  entity_required  True  → emit ONLY placements that carry an MSB EntityID (the
                          INTERACTIVE instances — Imp seals, Hero's Tomb statues; the
                          same model placed as decoration carries none).
                   False → emit every placement (Stakes are all real respawn points).
  category_wipe    True  → the feature OWNS a dedicated category (no other baked feature
                          shares it): drop ALL baked rows of that category when the disk
                          pass placed ≥1 (the bake is LOD-phantom-inflated — the disk _00
                          pass is authoritative, same pattern as live bosses).
                   False → the category is SHARED with other baked features (Hero's Tomb
                          shares WorldInteractables with Seal Puzzles): drop only the baked
                          twin sitting on a disk-placed cell, KEEPING the siblings.
  flag_rule        How the runtime derives the marker's graying flag (textDisableFlagId1) so
                   an activated/looted instance hides like the bake did. The flag itself is
                   NOT baked — it's resolved live from the mod's own files (no committed bake):
                     'none'            no flag (respawn points: Stakes never "complete").
                     'imp_seal'        flag = tile_base(area,gx,gz) + (entityId % 1000), and
                                       the rule also REJECTS placements whose entity suffix
                                       isn't a real seal {570,575,565,611} and picks the key
                                       label by suffix (565 = Imbued Sword Key, else Stonesword).
                     'hero_tomb_emevd' flag = the activated flag from EMEVD template 90005683,
                                       joined to the asset by EntityID (the mod's event\\*.emevd).
                     'seal_emevd'      flag = the activation flag from EMEVD template 90006051,
                                       joined by EntityID. SELF-GATES: a placement with no 90006051
                                       binding is decoration / non-seal use of the model → skipped.
  lod_scan         True  → the model's placements live ONLY in non-`_00` LOD supertiles, which the
                          runtime's `_00`-only asset enumeration skips. The world-feature pass adds a
                          targeted non-`_00` asset scan (load_lod_feature_assets) for these rows, taking
                          the marker tile from the cross-tile part-name prefix "m{AA}_{BB}_{CC}_00-…" (the
                          supertile stores the part relative to that fine tile). Set ONLY for models with
                          NO `_00` placements (else the `_00` + LOD copies double-count at offset spots) —
                          verify with tools/_probe_puzzle_tier.py / _probe_snow_statue.py.
                   False → normal `_00`-tile model (the default; the asset enumeration already sees it).
"""

WORLD_FEATURE_ASSETS = [
    # Stakes of Marika — respawn points, no entity, dedicated category. The "439" baked
    # rows were LOD-phantom-inflated; the disk _00 world-distinct count is ~219, so wipe.
    dict(aeg_row=99060, model='AEG099_060', category='WorldStakesOfMarika',
         text_id=900301540,        # tutorial text "Stakes of Marika"
         entity_required=False, category_wipe=True, flag_rule='none', lod_scan=False),

    # Imp / Seal Statues (the stone imp face you spend keys on). Two models, both seals;
    # the interactive seal carries a seal EntityID (decoration imps do not). Dedicated
    # category. The imp_seal rule derives the unlock flag arithmetically (so an opened seal
    # grays), filters to the 4 real seal entity-suffixes, and picks the key label per suffix.
    dict(aeg_row=27078, model='AEG027_078', category='WorldImpStatues',
         text_id=500008000,        # FMG: Stonesword Key (default; imp_seal rule overrides 565→Imbued)
         entity_required=True, category_wipe=True, flag_rule='imp_seal', lod_scan=False),
    dict(aeg_row=27079, model='AEG027_079', category='WorldImpStatues',
         text_id=500008000,
         entity_required=True, category_wipe=True, flag_rule='imp_seal', lod_scan=False),

    # Hero's Tomb instruction statues. Interactive instances carry an EntityID (the EMEVD
    # 90005683 template references it); decorative copies of the same model do not. They
    # SHARE WorldInteractables with Seal Puzzles, so cell-dedup (NOT category-wipe) — the
    # disk pass only reproduces the statues, the puzzles must stay baked. The hero_tomb_emevd
    # rule joins each statue's EntityID to the EMEVD activated flag so it hides once used.
    dict(aeg_row=99055, model='AEG099_055', category='WorldInteractables',
         text_id=800000000 + 7041,  # ActionButtonText[7041] "Examine statue" (runtime-localized)
         entity_required=True, category_wipe=False, flag_rule='hero_tomb_emevd', lod_scan=False),
    # AEG099_057 = a second model variant of the SAME Hero's Tomb instruction statue (template
    # 90005683). The disk pass keyed only on AEG099_055 → missed the 2 AEG099_057 statues (Caelid
    # m60_49_55 + m60_50_53, entities 1049551600/1050531600) which stayed baked. Same self-gated
    # hero_tomb_emevd join → only the EMEVD-bound statues emit (decorative copies have no flag).
    dict(aeg_row=99057, model='AEG099_057', category='WorldInteractables',
         text_id=800000000 + 7041,
         entity_required=True, category_wipe=False, flag_rule='hero_tomb_emevd', lod_scan=False),

    # Seal Puzzles — the AEG099_090 "Examine seal" object (the multi-seal puzzles that unlock
    # a fog door). 77/98 of the bake's seal interact-points. SHARES WorldInteractables with Hero's
    # Tomb → cell-dedup, NOT category-wipe (only the AEG099_090 cells drop their baked twin; specials
    # + Hero Tomb keep theirs). The seal_emevd rule joins each seal's EntityID to the EMEVD
    # template-90006051 activation flag and SELF-GATES: a placed AEG099_090 with no 90006051 binding
    # is decoration / non-seal use → skipped (stronger than entity_required, avoids phantom non-seal
    # placements of this common model).
    dict(aeg_row=99090, model='AEG099_090', category='WorldInteractables',
         text_id=800000000 + 9503,  # ActionButtonText[9503] "Examine seal" (runtime-localized)
         entity_required=True, category_wipe=False, flag_rule='seal_emevd', lod_scan=False),

    # "Extra puzzle" interact-points — bespoke (non-90006051) EMEVD events, same seal_emevd shape
    # (entity→lit-flag, self-gating). The per-event (entity,flag) offsets are registered in
    # msbe_parser kEmevdFlagTemplates (Sellia chalices 1049392302/303 + 1050392303; Siofra lanterns
    # 12022601/12022621). All carry the "Light flame" ActionButtonText[9520]. Both models are _00-tile
    # assets (chalices m60_49/50_39, lanterns m12_02) → the _00 asset enumeration sees them.
    dict(aeg_row=99047, model='AEG099_047', category='WorldInteractables',
         text_id=800000000 + 9520,  # ActionButtonText[9520] "Light flame" — Sellia chalices
         entity_required=True, category_wipe=False, flag_rule='seal_emevd', lod_scan=False),
    dict(aeg_row=237055, model='AEG237_055', category='WorldInteractables',
         text_id=800000000 + 9520,  # ActionButtonText[9520] "Light flame" — Siofra lanterns
         entity_required=True, category_wipe=False, flag_rule='seal_emevd', lod_scan=False),

    # Snow Town "Seal Release Statue" puzzle (4 statues, event 1048572370, action btn 9528). LOD-ONLY:
    # the AEG110_029 assets live SOLELY in the LOD supertile m60_24_28_01 (cross-tile part-name prefix
    # "m60_48_57_00-…" → marker tile m60_48_57), which the runtime's _00-only asset enumeration skips.
    # lod_scan=True adds a targeted non-_00 asset scan (load_lod_feature_assets) that feeds these into the
    # SAME seal_emevd self-gate. The lit flag (1048570370+) joins via EMEVD template 1048572370 (entity@X0_4,
    # flag@X8_4) registered in msbe_parser kEmevdFlagTemplates. Confirmed 4/4 by tools/_probe_snow_statue.py.
    dict(aeg_row=110029, model='AEG110_029', category='WorldInteractables',
         text_id=800000000 + 9520,  # ActionButtonText[9520] "Light flame" — matches the bake
         entity_required=True, category_wipe=False, flag_rule='seal_emevd', lod_scan=True),
]
