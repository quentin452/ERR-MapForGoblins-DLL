---
name: bestiary-killcount-idea
description: "IDÉE FEATURE (backlog, non planifiée) — bestiary/progression boss-ennemis; 2 niveaux: checklist-boss cheap via flags existants vs vrai kill-count = net-new"
metadata: 
  node_type: memory
  type: project
---

Idée feature proposée par <user> 2026-06-28 : un **bestiary boss/ennemis avec compteur**. N'existe PAS aujourd'hui (cherché repo+mémoire : zéro "bestiary"/"kill count" ; tous les hits = verbe "kills" ou flags "defeated"). Dans AUCUN plan/backlog actuel (9 items DX, 3 plans audités, scoreboard MapGenie). = feature neuve.

**Contexte moteur :** Elden Ring n'a PAS de bestiary natif ni de compteur de kills par ennemi (≠ Monster Hunter). Les mobs normaux respawn, non trackés individuellement. Ce qui existe = des flags "defeated" BINAIRES (vaincu/pas vaincu), one-shot, par boss / NPC nommé / invader.

**Déjà en place (adjacent, réutilisable) :** clearedEventFlagId (boss vaincu → grise/cache le marqueur), event 90005792 "[Common] Hostile NPC Defeated" (msbe_parser.cpp:473, generate_hostile_npcs.py), option "boss markers red + auto-hide once defeated". Liste des boss + flags = tools/generate_boss_list.py (textEnableFlagId4 + textId4=5120 "Defeated" → defeat flag/entity).

**DEUX NIVEAUX de faisabilité :**
1. ✅ CHEAP — **Checklist de progression boss "X/Y vaincus"** : lire les flags `defeated` qu'on lit DÉJÀ (read_event_flag sur la liste de generate_boss_list.py) → panneau F1 "progression boss" (vaincus/restants, par région). État binaire, pas de compteur. Faisable tout de suite, zéro RE nouvelle.
2. 🔴 NET-NEW — **Vrai kill-count par ennemi (mobs inclus)** : aucun support natif → hooker les events de mort/dégâts + compter + persister nous-mêmes. Gros chantier RE.

Si on l'attaque : commencer par le niveau 1 (réutilise l'infra boss/defeated existante, cf. [[map-data-obs-moore-enemies]] pour la taxonomie ennemis). Le niveau 2 dépendrait d'un hook de mort à RE.
