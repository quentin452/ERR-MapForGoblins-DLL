---
name: dx-bugs-backlog
description: "Backlog DX/bugs MapForGoblins notés par <user> (icônes invisibles, manette/F1, pause-in-game, cursor desync, Y-offset, clustering)"
metadata: 
  node_type: memory
  type: project
---

Backlog DX + bugs relevé par <user> le 2026-06-28 (à traiter plus tard, pas encore investigué).

1. **Icônes quasi-invisibles pour certaines couleurs (BUG SYSTÉMIQUE)** — ex. "Doigt racorni de sans éclat" (Withered Dappled Finger?) et "Stake of Marika". ⚠️ PRÉCISION <user> : le bug apparaît AUSSI sur les icônes de l'atlas CPU, donc ce N'EST PAS spécifique au pipeline DDS disque — c'est systémique au rendu. Dans certaines configs de couleur, combinées avec la worldmap ER affichée derrière, certaines icônes deviennent quasi invisibles. DEUX cas distincts : (1) icône trop petite ; (2) couleur de l'icône qui se fond dans le décor de la map ER derrière (contraste insuffisant). → piste fix : outline/contour, halo/ombre portée, ou taille minimale garantie — pas un problème de crop rect ni de sampling DDS. Touche [[dvdbnd-packed-reader]] mais aussi tout le rendu marker.
2. **Bug ER natif manette (DX upstream)** — quand on déplace le curseur à la souris puis qu'on repasse à la manette, ER devrait recentrer le curseur au milieu mais ne le fait pas. (Bug du jeu, pas le nôtre — DX à compenser éventuellement.) ➕ <user> veut aussi auto-switcher les hints de touche (manette/souris) selon le device actif → LIRE le flag "active input device" d'ER, cf. [[input-device-active-flag]] (= MÊME flag que le drift worldmap-manette ; recette CE memory-diff dans le brief RE).
3. **Bug DX chez nous — F1 inaccessible à la manette** — impossible d'ouvrir les menus via F1 (équivalent manette). Donc impossible de jouer MapForGoblins end-to-end uniquement à la manette. → besoin d'un binding manette pour l'ouverture du menu.
4. **Intégrer le mod "Pause in game" directement dans MapForGoblins** — une case en plus dans F1. Évite tout conflit de touche possible avec le mod externe.
5. **Setting ImGui "pause à l'ouverture de F1"** — quand on ouvre F1 (manette ou clavier), proposer un setting pour choisir si le jeu se met en pause automatiquement à l'ouverture des settings MapForGoblins. Hypothèse DX : quand le menu F1 est ouvert on ne veut pas forcément jouer en même temps → peut-être meilleure DX qu'une simple case ImGui à long terme. Lié au point 4.
6. **Desync curseur ImGui ↔ ER au restart de la map** — quand on restart la map, le curseur devrait être recentré au milieu pour que les ToolTips s'affichent au même endroit que le curseur ER. Lié à [[overlay-item-search-bar]] (cursor-locate = caméra map 2D).
7. **DX pour icône plus haut/bas que le joueur (axe Y)** — trouver une indication visuelle quand une icône sur la map est à un Y différent du joueur, pour que le joueur ne cherche pas au mauvais étage/altitude.
8. **Clustering d'icônes dépassé** — en mode icon clustering, beaucoup d'icônes ne sont toujours pas clusterisées. L'algo actuel (heuristique?) est dépassé → envisager un clustering par tiles plutôt que par heuristique. Voir [[overlay-render-perf-followups]] (spatial grid déjà envisagé).
9. **Mode "Distance adaptative clustering" à améliorer aussi** — corollaire du point 8.

---

## Followups détectés en jeu (2026-06-28)

F1. **Fuite d'icônes NATIVES overworld → underground après téléport Browser.** En se téléportant via le Browser DLC → underground, on voit les icônes natives ER de l'overworld (grottes/églises/ruines/donjons mineurs) S'AFFICHER dans la page underground. = ce sont les icônes du JEU (natives), pas nos markers ImGui (qui eux respectent le `group` = isDLC*2|isUG). Probablement un desync de page-state ER au téléport Browser. **Hypothèse <user> : auto-fixé plus tard** quand on aura retiré TOUTES les icônes natives au profit des icônes ImGui (qui filtrent par group). À garder en followup / re-tester après la migration native→ImGui. Lié [[category-icons-00solo-atlas]] (migration GPU/native icons), [[session-2026-06-23-map-icons]] (suppression draw-only des natives).

F2. **Locate/recherche ne recentre pas sur une cible dans le Fog of War.** Quand la recherche (cursor-locate) téléporte vers un objet situé dans le fog of war, le **pan s'arrête au bord de la zone de pan visible** et ne recentre PAS sur l'item cible (l'item reste hors-vue / en bord de carte). = le pan est **clampé aux bornes visibles** (panX/panZ bornés), donc une cible au-delà de cette limite (zone non explorée / fog) ne peut pas être centrée. Piste <user> : **bypasser la limite de pan / ajouter le support du PAN OOB (out-of-bounds)** pour autoriser le recentrage au-delà des bornes explorées. À regarder plus tard. Lié [[overlay-item-search-bar]] (cursor-locate / take_locate_pos = caméra map 2D), [[worldmap-unsearched-fog-mask]] (oracle fog), et le clamp de pan dans la projection (project_screen / panX-panZ, goblin_projection.hpp + map_renderer.cpp).
