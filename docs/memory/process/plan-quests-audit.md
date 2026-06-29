---
name: plan-quests-audit
description: "Verdict d'audit du plan feat/quests (Quest Browser automation + NPC map layer) — directionnel OK mais périmé + risqué"
metadata: 
  node_type: memory
  type: project
---

Audit (2026-06-28) du plan `docs/feat_quests_implementation_plan.md` (commit origin/feat/quests 2257ce6 = ancêtre de master ; pas de code, doc seul). Plan = (A) automatiser le Quest Browser via read/write Event Flags + (B) afficher le NPC/asset de l'étape active sur la map (QuestNpcLayer).

**VERDICT v1 : directionnellement bon, mais partiellement périmé (ignore l'infra déjà livrée), risqué sur A (écriture de flags = mutation save), sous-scopé sur la DATA.**

✅ **PLAN RÉÉCRIT EN v2 le 2026-06-28** (branche locale feat/quests, doc docs/feat_quests_implementation_plan.md, NON commité — <user> push lui-même). v2 corrige les 7 points : §0 tableau "infra à réconcilier" ; §5 retire l'émission legacy WorldQuestNPC (layer = seule productrice) ; §3 réutilise ent_enemy/ent_any via entity_world_pos() ; §4 READ-only par défaut + write derrière config::questAllowFlagWrite (cheat gate) + priorité par étape ; §2 data dérivée EMEVD/MSB (démo bootstrap seulement) ; §5 invalidation cache (epoch+triggers) ; chemins repo-relatifs ; §7.6 test write sur save jetable.

✅ Tient :
- Helpers existent : `goblin::ui::read_event_flag(id)` (goblin_inject.cpp:4390) + `goblin::markers::set_event_flag(id,val)` (goblin_markers.cpp:133).
- `entityId` déjà capturé dans DiskEnemy/DiskCollectible (loot_disk.hpp). MarkerLayer a bien les virtuals category()/visible()/markers() → QuestNpcLayer override OK.
- Section "Blind Spots" (hostilité Ranni / transition boss / time-of-day) = bonne, s'aligne sur fail_flag/fail_conclusion/questGreyOnDeath existants. hostility_flag proposé complète proprement.
- Ajouts de champs structs (progress_flag/entity_id/name_id/hostility_flag) = additifs trailing-default, cohérents.

⚠️ Périmé / sous-estimé :
1. IGNORE L'INFRA EXISTANTE. Déjà livré : catégorie `WorldQuestNPC` émise map_entry_layer.cpp:1891 ; gate quest-aware `questNpcQuestAware` map_renderer.cpp:985 ; Quest Browser complet ; progression par étape DÉJÀ persistée dans `config::questProgress` (blob keyé par nom NPC, qp_get/qp_set goblin_overlay.cpp:2608). L'UI marque déjà le legacy "superseded by the Quest Browser". Le plan crée un QuestNpcLayer + "exclut WorldQuestNPC du s_cat loop" SANS retirer la voie legacy → double-draw. Doit retirer explicitement le legacy.
2. DUPLICATION index entité→pos : plan crée g_enemies_by_entity_id/g_assets_by_entity_id, mais ent_enemy/ent_any déjà construits au prebuild map_entry_layer.cpp:722-742. Réutiliser/exposer, pas de globales parallèles.
3. 🔴 ÉCRITURE DE FLAGS = mutation save / cheat. qp_set→set_event_flag force des flags EMEVD que le moteur pose normalement APRÈS dialogue → peut soft-lock / sauter reward / trigger event. Reco : READ-only par défaut (auto-tick depuis flags, pas d'écriture), écriture opt-in derrière gate "cheat", test sur save jetable. La verif du plan ne teste PAS la non-corruption.
4. SOURCE HYBRIDE confuse : qp_get renvoie flag si progress_flag, sinon bit ini → étapes mixtes dans une même quête. Besoin priorité/UX explicite.
5. 🔴 SOUS-SCOPING = la DATA. Plan câble ~3 quêtes démo (Boc/Alexander/Thops) ; les 34 questlines ont besoin de progress_flag/entity_id FIABLES = le gros du coût. Hard-coder va à contre-courant du no-bake / single-source-of-truth du repo → dériver depuis EMEVD/MSB (outillage [[darkscript3-emevd-decompile]] + hook event-flag in-overlay) au lieu de hand-author.
6. Invalidation cache QuestNpcLayer : layer quête doit ré-évaluer l'étape active au changement de flag ; plan muet sur cadence rebuild/perf.
7. Cosmétique : chemins file://<wsl_home>/ (WSL) dans le doc, on est Windows.

Branches connexes en remote : origin/feat/quest-npc-layer (1 commit : générateur + tune FRIENDLY_TEAM_TYPES depuis ERR), origin/feat/quest-npc-filter (vide vs master). Voir [[plans-to-audit]].
