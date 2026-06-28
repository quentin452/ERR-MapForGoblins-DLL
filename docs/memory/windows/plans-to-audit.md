---
name: plans-to-audit
description: "3 plans à auditer puis remettre en mémoire plus tard (branches feat/quests, feat/dx-bugs-backlog, feat/spatial-grid-opti)"
metadata: 
  node_type: memory
  type: project
---

<user> (2026-06-28) : **3 plans à auditer** puis remettre en mémoire — ✅ LES 3 SONT AUDITÉS. ⚠️ Noms de branches réels : `feat/quests` mais `feature/dx-bugs-backlog` + `feature/spatial-grid-opti` (préfixe `feature/`, pas `feat/`). Docs : docs/feat_quests_implementation_plan.md, docs/dx_bugs_backlog_plan.md, docs/spatial_grid_opti_plan.md.

1. **feat/quests** — ✅ AUDITÉ, verdict [[plan-quests-audit]] (directionnel OK mais périmé/risqué/sous-scopé). PLAN RÉÉCRIT EN v2 + commité (6f0b6fb sur branche locale feat/quests, non pushé).
2. **feature/dx-bugs-backlog** — ✅ AUDITÉ + RÉÉCRIT v2 (commit b5b4d53). Verdict [[plan-dx-bugs-audit]]. Couvre le backlog [[dx-bugs-backlog]].
3. **feature/spatial-grid-opti** — ✅ AUDITÉ + RÉÉCRIT v2 (commit 6e08f9c). Verdict [[plan-spatial-grid-audit]]. À FAIRE EN PREMIER.

✅ LES 3 PLANS SONT RÉÉCRITS EN v2 ET COMMITÉS (branches locales, NON pushés — <user> push lui-même) : feat/quests 6f0b6fb, feature/spatial-grid-opti 6e08f9c, feature/dx-bugs-backlog b5b4d53.

SÉQUENÇAGE recommandé : spatial-grid D'ABORD (fondation) → DX clustering[8,9] réutilise les cellules → reste DX en PRs indépendantes → quests. Les 3 features (incl. QuestNpcLayer) partagent UNE discipline d'invalidation grille/cache.

⭐ CONTEXTE SCALING (<user> 2026-06-28) : ~8000 markers aujourd'hui, mais >10k dès qu'on câble les 31 types d'icônes "missing" présents dans MapGenie (docs/coverage_vs_mapgenie.md) → justifie le spatial-grid O(visible).
