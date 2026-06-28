---
name: plan-dx-bugs-audit
description: "Verdict d'audit du plan feature/dx-bugs-backlog (9 items DX) — bonne couverture mais scope énorme + pause QPC risquée"
metadata: 
  node_type: memory
  type: project
---

Audit (2026-06-28) de `docs/dx_bugs_backlog_plan.md` (branche origin/**feature**/dx-bugs-backlog, PAS feat/). Couvre les 9 items de [[dx-bugs-backlog]].

✅ **PLAN RÉÉCRIT v2 + commité 2026-06-28 (commit b5b4d53, branche locale feature/dx-bugs-backlog, non pushé).** v2 : DÉCOUPÉ EN 5 PRs (A icônes / B Y-offset / C manette+curseur / D pause / E clustering) ; pause QPC-global REJETÉE par défaut → RE-spike d'abord pour trouver le timestep/flag pause du jeu (QPC = fallback scopé only) ; clustering[8,9] séquencé APRÈS + SUR feature/spatial-grid-opti (cellules grille = piles tile, scale aux >10k markers attendus avec les 31 cats MapGenie) ; réutilise parse_gamepad_combo+GamepadMask+SetCursorPos hooké ; noms corrigés ; chemins repo-relatifs.

**VERDICT : bonne couverture (évite correctement le piège sampling DDS), infra réelle, mais SCOPE ÉNORME (9 features/1 PR → découper), PAUSE QPC RISQUÉE, dérive de noms, clustering à unifier+séquencer après le spatial-grid.**

✅ Tient :
- Item 1 icônes invisibles : min-size + 1 disque sombre de fond, ÉVITE de re-sampler la DDS → colle EXACTEMENT à la reco [[dx-bugs-backlog]] (outline/halo/taille-min, PAS sampling).
- Item 7 Y-offset : get_player_world_pos(x,y,z) existe (goblin_inject.hpp:57) ; Marker n'a que worldX/worldZ → ajout worldY correct ; badge ▲/▼.
- Infra hook réelle : MinHook + Present hook + WndProc hook + SetCursorPos DÉJÀ hooké (o_set_cursor_pos, goblin_overlay.cpp). Recentrage curseur (items 2/6) faisable.
- Gamepad à moitié câblé : parse_gamepad_combo("Y+R3") + IniType GamepadMask DÉJÀ là (goblin_config.cpp:225, schema). overlayToggleKey (F1 clavier) existe+utilisé (overlay.cpp:2820). AUCUN XInputGetState encore → polling manette = nouveau. RÉUTILISER le parsing, pas réinventer.
- Caveat EAC/offline OK.

⚠️ Problèmes :
1. 🔴 SCOPE : 9 features/1 plan = PR ingérable. Découper : (A) icônes[1] (B) input/manette/curseur[2+3+6] (C) pause[4+5] (D) clustering[8+9 dépend spatial-grid] (E) Y-offset[7].
2. 🔴 PAUSE via hook GLOBAL de QueryPerformanceCounter = lourd/risqué : gèle QPC process-wide (audio, timing du mod, anim/physique) ; timer virtuel à tick constant → dt=0 ailleurs → div0/stalls. Mods pause ER établis patchent le timestep/vitesse OU le flag pause/cutscene du jeu. ÉVALUER l'approche ciblée avant le marteau QPC. = LE point à valider.
3. Dérive noms : référence MapEntryLayer::push_marker (= fonction LIBRE push_marker) et GraceLayer::collect (n'existe pas — c'est markers() qui build cache_). worldY → dans push_marker libre / là où cache_ est build.
4. Items 8-9 clustering CHEVAUCHENT le plan spatial-grid : même clé tile (group<<20)|(gx<<10)|gz. UNIFIER + séquencer APRÈS spatial-grid (les cellules = les clusters). Clustering actuel = par cluster_key (nearest-grace), groupé via unordered_map<int,vector<int>> dans draw_clusters (map_renderer.cpp:533).
5. Chemins WSL file://<wsl_home>/ à corriger.

Séquençage transverse (voir [[plan-spatial-grid-audit]]) : spatial-grid D'ABORD → DX clustering ensuite → reste DX en PRs indépendantes. Les 3 features (incl. QuestNpcLayer de [[plan-quests-audit]]) partagent UNE discipline d'invalidation grille/cache.
