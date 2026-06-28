# Memory index — ERR-MapForGoblins-DLL

## Reconciliation 2026-06-28

Imported into a Linux checkout at `master` = `931438d`. Treat this archive as historical memory; prefer
committed repo state and current docs when there is a conflict. The old `next-session-resume` entry below
is stale in this checkout: `feat/quests`, `feature/spatial-grid-opti`, and `feature/dx-bugs-backlog` are
not ahead of `master`; `origin/feat/dvdbnd-packed-reader` exists remotely but is not merged into `master`.

- ⚠️ [next-session-resume](next-session-resume.md) — STALE IMPORT SNAPSHOT: old point de reprise for a different checkout state. Do not follow blindly; see its 2026-06-28 import note first.

- ▶ [plans-to-audit](plans-to-audit.md) — 3 plans audités (feat/quests, feature/dx-bugs-backlog, feature/spatial-grid-opti); séquençage + reste-à-faire dedans
- [plan-quests-audit](plan-quests-audit.md) — verdict feat/quests: directionnel OK mais ignore l'infra livrée + écriture-flags risquée + data sous-scopée; plan réécrit v2 (commit 6f0b6fb, non pushé)
- [plan-dx-bugs-audit](plan-dx-bugs-audit.md) — verdict feature/dx-bugs-backlog (9 items): bonne couverture mais scope énorme à découper + pause QPC globale risquée + clustering à unifier après spatial-grid
- [plan-spatial-grid-audit](plan-spatial-grid-audit.md) — verdict feature/spatial-grid-opti: le plus solide (TODO perf [[overlay-render-perf-followups]]), à faire EN PREMIER; gaps world↔map-space, invalidation grille, mesurer le gain
- [bestiary-killcount-idea](bestiary-killcount-idea.md) — IDÉE feature (non planifiée): bestiary boss/ennemis; niv.1 checklist-boss "X/Y vaincus" cheap via flags defeated existants, niv.2 vrai kill-count = net-new (hook de mort à RE)
- ▶ [dx-bugs-backlog](dx-bugs-backlog.md) — backlog 9 items DX/bugs (<user> 2026-06-28): icônes invisibles systémiques, manette/F1, pause-in-game, desync curseur, Y-offset, clustering par tiles
- [input-device-active-flag](input-device-active-flag.md) — flag moteur ER "active input device" (manette/souris) = même source pour hint-switch auto + recentrage curseur + fix drift worldmap-manette; brief RE windows_gamepad_input_device_re_prompt.md, recette CE memory-diff
- [map-data-obs-moore-enemies](map-data-obs-moore-enemies.md) — NE PAS rechasser comme bugs: Moore Bell Bearing≠Black Syrup (2 items), 3 entités Moore (multi-placement NPC→QuestNpcLayer), Furnace Golem=Elite Enemy non câblé (≠ les 800 unclassified=items)

- ▶ [category-icons-00solo-atlas](category-icons-00solo-atlas.md) — RESUME item-icon GPU: layout SOLVED (decompress sblytbnd→SB_Icon_*.layout→rects, wired+validated); NEXT GAP#2 = extraire la DDS du sheet depuis l'archive disque→crop→draw
- ▶ [item-icons-table-deleted](item-icons-table-deleted.md) — ITEM_ICONS supprimé (branch feat/drop-item-icons-table, validé): les FRAMES étaient des map-point frames par catégorie, pas des iconIds; category→icon déjà live. LESSON: tracer ce qu'une valeur bakée EST avant de la live-read
- [er-item-taxonomy-sortgroupid](er-item-taxonomy-sortgroupid.md) — classification ER native = goodsType(@+0x3e)+sortGroupId(@+0x72), live-readable, drift-free
- [item-classification-guard](item-classification-guard.md) — [ITEMCLASS] census + docs/item_classification.md = garde-fou régression item→catégorie (git-diff le doc)
- ▶ [param-offset-source-of-truth](param-offset-source-of-truth.md) — RESUME live-RPM single-source: pas de paramdef runtime; Paramdex drifte→bytes live = vérité; NEXT = lire les offsets DIRECTEMENT du code exe (AOB→displacement), zéro constante
- [phase3-taxonomy-map-validated](phase3-taxonomy-map-validated.md) — classifier (gType,sg)→Category SHIPPED+validé; ITEM_ICONS icon-only + exceptions(133); garder tools/taxonomy_classifier.py en phase avec goblin_inject.cpp
- [overlay-item-search-bar](overlay-item-search-bar.md) — F1 "Find item/object" (commit dd566c7): liste + ring-highlight + cursor-locate; API set_item_search/take_locate_pos; calibration cursor possible
- [worldmap-unsearched-fog-mask](worldmap-unsearched-fog-mask.md) — fog oracle = FUN_140886560(VM,layer,tileId)→flags, révélé si (flags&0x17fff)==0; table VM+0x288; tileId=group*10000+gx*100+gz
- ▶ [handoff-loot-from-real-files](handoff-loot-from-real-files.md) — START loot-from-real-MSB: wired+validé (4 profils) + 3-tier map-dir (feat/map-open-probe); caveats dedans
- [imgui-unicode-font](imgui-unicode-font.md) — font overlay: ImGui défaut=Latin-1 only; fix=merge font système (818bac4); TODO bundler un TTF dans la DLL
- [aeg-collectible-source](aeg-collectible-source.md) — source no-bake collectibles ERR: AEG099_8xx + AssetEnvGeomParam.pickUpItemLotParamId(@+0xb8)→ItemLot→goods; SHIPPED loot_collectibles (59f4bf3)
- [msbe-dummyasset-filter](msbe-dummyasset-filter.md) — filtre DummyAsset disk-loot (MSBE part-type @+0x0c 13/9), 3 reachable_dummy, 21 faux-positifs
- [resolve-loot-flag-dlc-bug](resolve-loot-flag-dlc-bug.md) — resolve_loot_flag >=0x40000000 drop à tort le loot DLC one-time; fix=EventFlagMan group query, validé 2026-06-25; wire L4145+L4207
- [nobake-coverage-scoreboard](nobake-coverage-scoreboard.md) — provenance par marker→[COVERAGE]+docs/nobake_scoreboard.md; baked 4744→16 (mergé master); 16 restants=positionless-IRREDUCIBLE; NEXT=Phase 2; RULE: lire les feeds runtime, pas grep goblin_map_data offline
- [nobake-endgame-roadmap](nobake-endgame-roadmap.md) — plan 3 phases: (1) baked→0, (2) delete goblin_map_data+deps, (3) refactor offset-free (paramdef+parser disque+RTTI/AOB)
- [delete-generate-data-path](delete-generate-data-path.md) — ✅ intermédiaire _map_entries_full.cpp éliminé (feat/phase2-drop-intermediate, validation in-game PENDING); generate_data PAS deletable (5 tables live); MSBE Part modelIndex=u32@+0x14
- ▶ [disk-parser-coverage-gaps](disk-parser-coverage-gaps.md) — RESUME: items réels manquants (Ghostflame Torch…), le parser disque re-copie EMEVD/MSBE et rate boss-EMEVD/treasure; NEXT=single source of truth parser; test F1-search
- [residual-irreducible-strategy](residual-irreducible-strategy.md) — lens: un résidu "irréductible" = question ouverte, classer (A) gap parse-disque vs (B) chemin runtime inconnu; nommer le blocker avant d'accepter
- [world-feature-msb-identities](world-feature-msb-identities.md) — identité MSB (AEG/Region/param) de CHAQUE World feature; déjà dataminé tools/generate_*.py; source no-bake du backlog ~1300-baked
- [msbe-enemy-loot-offsets](msbe-enemy-loot-offsets.md) — offsets pin pour pass ENEMY no-bake: MSB Enemy type==2, NpcParamID=*(*(part+0x68)+0x0c), pos@+0x20; NpcParam itemLot @0x30/@0x34
- [overlay-render-perf-followups](overlay-render-perf-followups.md) — 2 TODOs perf boucle markers (O(n)/frame, ~8477): #1 cache frame-cohérent, #2 spatial grid O(visible)
- [collected-refresh-proton-perf](collected-refresh-proton-perf.md) — ✅ root-cause fixé (83a723f): RPM-to-self = flood IPC wineserver→stutter 20fps PROTON; fix=read DIRECT in-process via __try; ⚠️ bit7 vs bit1 graying-bug possible
- [fragment-gate-maplist-gap](fragment-gate-maplist-gap.md) — fuite require_map_fragments: trous MapList comblés par disk pass→flag=0; fixé (c5399dd) fallback voisin-majorité; followup dist>1 (low pri)
- [virtualfs-alias-modroot-anchor](virtualfs-alias-modroot-anchor.md) — alias virtual-FS ER pour auto-locate mod-map: mgr 0x48464a8 + setter 0x1ed7af0; verdict A/B + recette runtime
- ▶ [dvdbnd-packed-reader](dvdbnd-packed-reader.md) — reader packed Strategy-C shipped and later in-game validated in the detailed note; import checkout has only `origin/feat/dvdbnd-packed-reader` (remote, not merged into `master`): parse Data*.bhd/.bdt (RSA BCrypt, BHD5, hash prime 0x85); REUSE pour GAP#2 DDS
- [runtime-msb-resident-plan](runtime-msb-resident-plan.md) — dépendre des vrais fichiers mod: layout MSBE resident mappé+prouvé (Treasure type4→itemLotId@td+0x10, partIndex@+0x08→pos@+0x20); NEXT=parser C++ MSBE
- [fieldins-pool-registry-re](fieldins-pool-registry-re.md) — FieldIns/loot RE: walker loaded-loot SOLVED (MapIns er+0x2a8d6d8→node@+0x460); scope=loot spawné/ouvert/placé only

- [workflow-preferences](workflow-preferences.md) — confirmer les décisions, feature branches, il push & runtime-teste
- [windows-tooling-gotchas](windows-tooling-gotchas.md) — cmd/.bat via PowerShell tool, overlays FS sandbox, redirects périmés, D:\ pour gros fichiers
- [build-toolchain-clang-xwin](build-toolchain-clang-xwin.md) — build+deploy la DLL (sans MSVC): clang-cl(scoop)+xwin+ninja, cmds & gotchas
- [mapforgoblins-pipeline-setup](mapforgoblins-pipeline-setup.md) — setup pipeline 4-profils sur cette machine Windows (chemins, oo2core, run cmd)
- [rpm-live-memory-tooling](rpm-live-memory-tooling.md) — lire la mémoire live eldenring.exe via Python ctypes RPM (<ghidra_scripts>\*.py); loop static→CE→RPM
- [ghidra-re-tooling](ghidra-re-tooling.md) — outils Ghidra réutilisables: grep tools/ghidra/rtti_index.txt + query.java; AVANT d'écrire un find_xxx.java
- [ghidra-worldmap-re](ghidra-worldmap-re.md) — projet <ghidra_project> + log RE world-map/menu (projection, LegacyConv, item-icon, grace pins); ⚠️ trust the latest dated entry
- [re-offset-validation](re-offset-validation.md) — JAMAIS hand-derive un offset; pin empirique (bytes ancrés, samples +/- vs SoulsFormats) + sanity-check; leçon isEnableRepick bit5-vs-6
- [darkscript3-emevd-decompile](darkscript3-emevd-decompile.md) — DarkScript3 CLI batch-decompile ERR EMEVD→JS grep-able; validation des 3 death flags (Iji suspect)
- [er-console-mod](er-console-mod.md) — Nexus 9365 console mod comme outil de readout coords; toggle=ù; coords block-local vs tp
- [er-prologue-flag](er-prologue-flag.md) — event flag 120 = prologue (Chapel m19) fait; gate la map ImGui sur !read_event_flag(120)
- [player-pos-static-unreliable](player-pos-static-unreliable.md) — offsets player-pos statiques faux sur 2.6.2.0; player pos SOLVED = [[WCM+0x1E508]+0x6C0]
- [er-shutdown-crash-noise](er-shutdown-crash-noise.md) — crash_*.txt à eldenring.exe +0x1EB9999 (no DLL frame) = teardown Exit/Alt+F4 d'ER, ignorer sauf si fault_module = notre DLL
- [runtime-icon-coverage](runtime-icon-coverage.md) — RESUME per-item icon: shipped; plafond runtime prouvé (streams, binding virtuel); NEXT=bake offline OU bloquer l'evict queue
- [session-2026-06-23-map-icons](session-2026-06-23-map-icons.md) — suppression draw-only des graces (shipped) + map-point iconId→rect depuis RAM repo (data done, render TODO)
