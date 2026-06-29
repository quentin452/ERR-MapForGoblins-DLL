---
name: map-data-obs-moore-enemies
description: "NE PAS rechasser comme bugs — Moore Bell Bearing vs Black Syrup (2 items distincts), 3 entités Moore (multi-placement NPC), Furnace Golem=Elite Enemy non câblé"
metadata: 
  node_type: memory
  type: project
---

Observations <user> 2026-06-28 en jouant (worldmap), toutes EXPLIQUÉES — ne pas les traiter comme des bugs à corriger à l'aveugle :

**Furnace Golem invisible / catégories d'ennemis (≠ les 800 unclassified).**
docs/coverage_vs_mapgenie.md : `Elite Enemy` (MG 184) = ❌ NOT WIRED → c'est la catégorie du **Furnace Golem** (d'où invisible). `Enemy` (MG 82, mobs normaux) = ❌ NOT WIRED. `Bosses` (214/217) = ✅ wired (Godrick s'affiche). NPC mini-boss via Enemies/NpcParam. ⚠️ Les ~800/883 unclassified sont des ITEMS (goods sans catégorie, [[disk-parser-coverage-gaps]]), RIEN à voir avec les enemies. FUTUR = audit taxonomie d'ennemis pour câbler Elite Enemy + Enemy (2 des 31 catégories MapGenie non câblées).

**"Moore's Bell Bearing" vs "Black Syrup" = PAS un rename, PAS un changement ERR.** Deux items DIFFÉRENTS liés à Moore au Main Gate Cross : Black Syrup = récompense de quête que Moore donne (étape "Thiollier's syrup", goblin_quest_steps.cpp:269) ; Moore's Bell Bearing (id 502008900, goblin_name_aliases_en.cpp:2019) = drop à la MORT de Moore. MapGenie marque le Black Syrup ; notre pass disque fait surfacer le LOT du Bell Bearing (death-drop) placé à l'emplacement de Moore → différence de SOURCE de loot, pas de nom. (Quirk lié : on affiche un death-drop comme pickup statique = même trou "récompense NPC" que les ennemis.)

**Plusieurs entités "Moore" dans le DLC = PAS un changement ERR.** Les 3 "Moore" (700141500/501/502, goblin_name_aliases_en.cpp:2267-69) = multiples placements MSB du MÊME NPC selon phases de quête/map states (multi-placement connu des quest NPCs, docs/re/windows_quest_npc_prompt.md). MapGenie n'en montre qu'un (ou aucun pour marchands amicaux). → exactement ce que le QuestNpcLayer de [[plan-quests-audit]] (feat/quests) doit régler : n'afficher que le placement actif. Déjà sur la roadmap.

**Les ~800 unclassified sont-ils trackés ? OUI** — nobake_scoreboard.md L184 (unclassified 883) + [[disk-parser-coverage-gaps]].

**✅ VERROUILLÉ 2026-06-28 — Black Syrup invisible = NORMAL, PAS un bug parser/classification.** Texte MapGenie exact : *"After meeting Thiollier, speak to Moore to RECEIVE Black Syrup to give to Thiollier."* → c'est un **don de dialogue de quête** (sous-cas 1 de [[disk-parser-coverage-gaps]] : sans position monde, aucun lot à parser). Le pin MapGenie = note éditoriale de quête au NPC. Chez nous c'est DÉJÀ modélisé comme **étape de quête** (goblin_quest_steps.cpp:269, "Thiollier's syrup"), pas comme marqueur. Pour avoir le pin façon MapGenie = `QuestNpcLayer` de [[plan-quests-audit]] (étape active → épingle Moore via entity_id). RIEN à corriger côté loot parser. NE PAS rechasser.
