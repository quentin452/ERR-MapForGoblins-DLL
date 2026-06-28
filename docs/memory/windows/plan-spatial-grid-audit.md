---
name: plan-spatial-grid-audit
description: "Verdict d'audit du plan feature/spatial-grid-opti — le plus solide des 3 (implémente un TODO perf documenté), à faire EN PREMIER"
metadata: 
  node_type: memory
  type: project
---

Audit (2026-06-28) de `docs/spatial_grid_opti_plan.md` (branche origin/**feature**/spatial-grid-opti, PAS feat/ ; 2 commits : plan + "Phase 2 Quadtree"). 2D spatial grid (cellule 256m = tile ER) pour culler les markers off-screen → boucle de rendu O(N)→O(visible).

✅ **PLAN RÉÉCRIT v2 + commité 2026-06-28 (commit 6e08f9c, branche locale feature/spatial-grid-opti, non pushé).** v2 corrige : keying en MAP-SPACE (option B → query = unproject_local pur des 4 coins, pas d'inverse world→mapspace) ; noms corrigés (markers()/cache_ pas finalize/collect) ; règle d'invalidation grille=rebuild-on-cache-rebuild (anti dangling Marker* vs QuestNpcLayer dynamique) ; alimenter les DEUX passes ; mesure-first. ⭐ Ajout du contexte SCALING : ~8000 markers AUJOURD'HUI → **>10k dès qu'on câble les 31 catégories MapGenie "missing" (docs/coverage_vs_mapgenie.md, backlog feature)** → c'est CE qui justifie O(visible) + le quadtree différé.

**VERDICT : LE PLUS SOLIDE des 3. Implémente un TODO perf déjà documenté, infra présente (bench + inverse projection), design sain. À FAIRE EN PREMIER = fondation des items clustering du DX. Gaps : conversion world↔map-space, dérive de noms, invalidation grille, alimenter les DEUX passes.**

✅ Tient :
- Bottleneck réel + documenté : O(N) sur ~8477 markers/frame = littéralement le TODO #2 de [[overlay-render-perf-followups]] ("spatial grid for O(visible)").
- GOBLIN_BENCH_QUIET("render.worldmap.markers") existe (map_renderer.cpp) → mesure avant/après.
- Inverse projection EXISTE : unproject_local (goblin_projection.hpp:131, marker=(screen+pan)/zoom) → visible_world_bounds faisable. DÉ-RISQUÉ.
- Cellule 256 = structure coords ER (worldX=gridX*256+posX). Grille par-layer = raisonnable.
- Render loop confirmé : for(auto*L:layers){ for(const Marker&m:L->markers()) ... } (map_renderer.cpp:1223).

⚠️ Problèmes :
1. Double conversion world↔map-space ÉLUDÉE : markers stockent worldX/worldZ (unifié) mais project_screen consomme du map-space (mU,mV) via étape world→map-space (map_renderer.cpp:370). unproject_local rend du map-space → visible_world_bounds doit AUSSI inverser world→map-space, OU bucketiser la grille en map-space (1×/marker au build). Solvable, à expliciter.
2. Dérive noms (idem DX) : MapEntryLayer::finalize / GraceLayer::collect N'EXISTENT PAS — c'est markers() qui build cache_ en lazy (mutable std::vector<Marker> cache_, call_once). Grille construite à côté de cache_ + INVALIDÉE quand cache_ rebuild.
3. POINTEURS PENDANTS : grille stocke const Marker* dans cache_. OK si cache_ build-once-stable, MAIS le QuestNpcLayer de [[plan-quests-audit]] est DYNAMIQUE (rebuild sur flag) → grille doit rebuild avec lui. Coordination inter-features.
4. Passe clustering aussi O(N) (render.worldmap.clusters, groupage cluster_key). Le plan optimise la boucle draw mais confirmer que les DEUX passes (markers + clusters) sont alimentées par la grille, sinon moitié du gain perdue.
5. Gain estimé (2.5-4ms→<0.2ms) NON vérifié : la projection est DÉJÀ cachée (mémoire) → le vrai coût CPU peut être les checks gate/catégorie par marker, pas la projection. MESURER d'abord.
6. Quadtree Phase 2 = prématuré (50k markers hypothétiques), OK note future basse pri.
7. Chemins WSL.

Séquençage transverse : spatial-grid D'ABORD (fondation) → DX clustering[8,9] réutilise les cellules → reste DX indépendant. Voir [[plan-dx-bugs-audit]] + [[plan-quests-audit]]. Les 3 features partagent UNE discipline d'invalidation grille/cache.
