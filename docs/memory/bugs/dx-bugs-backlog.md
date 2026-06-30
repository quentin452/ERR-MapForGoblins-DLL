---
name: dx-bugs-backlog
description: "Backlog DX/bugs MapForGoblins notés par <user> (icônes invisibles, manette/F1, pause-in-game, cursor desync, Y-offset, clustering)"
metadata: 
  node_type: memory
  type: project
---

Backlog DX + bugs relevé par <user> le 2026-06-28 (à traiter plus tard, pas encore investigué).

1. ✅ **FIXED 2026-06-30** (`feat/dx-icon-visibility`, PR A) — legibility pass in `draw_marker`: minimum
   on-screen size (case 1) + a dark backing disc gated to *small* item/rep icons for contrast (case 2);
   native map symbols left untouched (no halo). Config `icon_legibility` / `icon_min_half_px`. Visually
   confirmed. Original report kept below.
   **Icônes quasi-invisibles pour certaines couleurs (BUG SYSTÉMIQUE)** — ex. "Doigt racorni de sans éclat" (Withered Dappled Finger?) et "Stake of Marika". ⚠️ PRÉCISION <user> : le bug apparaît AUSSI sur les icônes de l'atlas CPU, donc ce N'EST PAS spécifique au pipeline DDS disque — c'est systémique au rendu. Dans certaines configs de couleur, combinées avec la worldmap ER affichée derrière, certaines icônes deviennent quasi invisibles. DEUX cas distincts : (1) icône trop petite ; (2) couleur de l'icône qui se fond dans le décor de la map ER derrière (contraste insuffisant). → piste fix : outline/contour, halo/ombre portée, ou taille minimale garantie — pas un problème de crop rect ni de sampling DDS. Touche [[dvdbnd-packed-reader]] mais aussi tout le rendu marker.
2. **Bug ER natif manette (DX upstream)** — quand on déplace le curseur à la souris puis qu'on repasse à la manette, ER devrait recentrer le curseur au milieu mais ne le fait pas. (Bug du jeu, pas le nôtre — DX à compenser éventuellement.) ➕ <user> veut aussi auto-switcher les hints de touche (manette/souris) selon le device actif → LIRE le flag "active input device" d'ER, cf. [[input-device-active-flag]] (= MÊME flag que le drift worldmap-manette ; recette CE memory-diff dans le brief RE).
3. **Bug DX chez nous — F1 inaccessible à la manette** — impossible d'ouvrir les menus via F1 (équivalent manette). Donc impossible de jouer MapForGoblins end-to-end uniquement à la manette. → besoin d'un binding manette pour l'ouverture du menu.
4. **Intégrer le mod "Pause in game" directement dans MapForGoblins** — une case en plus dans F1. Évite tout conflit de touche possible avec le mod externe.
5. **Setting ImGui "pause à l'ouverture de F1"** — quand on ouvre F1 (manette ou clavier), proposer un setting pour choisir si le jeu se met en pause automatiquement à l'ouverture des settings MapForGoblins. Hypothèse DX : quand le menu F1 est ouvert on ne veut pas forcément jouer en même temps → peut-être meilleure DX qu'une simple case ImGui à long terme. Lié au point 4.
6. **Desync curseur ImGui ↔ ER au restart de la map** — quand on restart la map, le curseur devrait être recentré au milieu pour que les ToolTips s'affichent au même endroit que le curseur ER. Lié à [[overlay-item-search-bar]] (cursor-locate = caméra map 2D).
7. ✅ **FIXED 2026-06-30** (`feat/dx-altitude-cue`, PR B) — ▲/▼ triangle badge (AddTriangleFilled, no
   font dep) when a marker's Y differs from the player's, gated to the player's current map layer
   (group match) so cross-map viewing doesn't badge everything. MSB block-local Y (`pos[1]`) threaded
   onto markers (added posY to DiskTreasure/DiskEnemy + captured in the parser). F1 toggle + ini
   `altitude_cue`/`altitude_deadzone`. Validated in-game (direction tracks player altitude). FOLLOW-UP:
   3 niche sources still drop posY (ReforgedRune/EmberPieces `bpos`, scarab/maps-pass `pos`, sibling-LOD
   `lit`) → those markers get no badge; thread if needed.
   **DX pour icône plus haut/bas que le joueur (axe Y)** — trouver une indication visuelle quand une icône sur la map est à un Y différent du joueur, pour que le joueur ne cherche pas au mauvais étage/altitude.
8. ✅ **FIXED 2026-06-30** (`feat/spatial-grid`, PR E) — replaced the nearest-grace heuristic with
   **tile-based clustering**: group by the marker's map-space 256-unit tile (+ map layer, `spatial_grid.hpp`).
   Root causes of the "icônes pas clusterisées" found via diagnostics: (a) the old `cluster_key>=0`
   (grace-proximity) gate excluded grace-less items — removed; (b) baked-fallback underground positions
   scattered across tiles — clustering now uses live-projected map-space only; (c) the size threshold was
   gating PLAIN clustering — it's now adaptive-only (plain clustering piles any co-located tile, thr=1).
   Graces never pile (vanilla parity). **Clustering dépassé** — original report below.
9. ✅ **FIXED 2026-06-30** (same) — distance-adaptive ramp recomputed in map-space, gated to the player's
   own map layer; near=detail, far=denser. Corollaire du point 8, resolved with it.

---

## Followups détectés en jeu (2026-06-28)

F1. **Fuite d'icônes NATIVES overworld → underground après téléport Browser.** En se téléportant via le Browser DLC → underground, on voit les icônes natives ER de l'overworld (grottes/églises/ruines/donjons mineurs) S'AFFICHER dans la page underground. = ce sont les icônes du JEU (natives), pas nos markers ImGui (qui eux respectent le `group` = isDLC*2|isUG). Probablement un desync de page-state ER au téléport Browser. **Hypothèse <user> : auto-fixé plus tard** quand on aura retiré TOUTES les icônes natives au profit des icônes ImGui (qui filtrent par group). À garder en followup / re-tester après la migration native→ImGui. Lié [[category-icons-00solo-atlas]] (migration GPU/native icons), [[session-2026-06-23-map-icons]] (suppression draw-only des natives).

F2. **Locate/recherche ne recentre pas sur une cible dans le Fog of War.** Quand la recherche (cursor-locate) téléporte vers un objet situé dans le fog of war, le **pan s'arrête au bord de la zone de pan visible** et ne recentre PAS sur l'item cible (l'item reste hors-vue / en bord de carte). = le pan est **clampé aux bornes visibles** (panX/panZ bornés), donc une cible au-delà de cette limite (zone non explorée / fog) ne peut pas être centrée. Piste <user> : **bypasser la limite de pan / ajouter le support du PAN OOB (out-of-bounds)** pour autoriser le recentrage au-delà des bornes explorées. À regarder plus tard. Lié [[overlay-item-search-bar]] (cursor-locate / take_locate_pos = caméra map 2D), [[worldmap-unsearched-fog-mask]] (oracle fog), et le clamp de pan dans la projection (project_screen / panX-panZ, goblin_projection.hpp + map_renderer.cpp).

---

## Review-2 — relevé <user> le 2026-06-29 (à traiter plus tard)

10. **Heuristique visibilité RequireFragment + Region perd des icônes.** En cliquant sur une région (Region-click → affichage des objets de cette région) ET avec `require_map_fragments`, l'heuristique actuelle laisse certaines icônes hors de la plage de visibilité Region et hors de la plage RequireFragment → des MapForGoblins markers qui devraient s'afficher ne s'affichent pas. À investiguer (même famille que le clustering heuristique du point 8 / le gate fragment). Lié [[fragment-gate-maplist-gap]], [[worldmap-tile-fog-re]].
11. **Double-draw non-déterministe (F1 + minimap).** Parfois F1 ouvre DEUX fenêtres MapForGoblins : une déplaçable + une figée qui réplique le rendu (XXX fps mais bloquée). La minimap a aussi un double-draw similaire. **Non-déterministe** : selon les instances de jeu, parfois 1 seul layer, parfois 2. Hypothèse : double initialisation / double hook du loop de rendu (Present hooké 2×) ou layer dupliqué. Probablement lié au point 12 (inputs souris qui passent à travers). À reproduire + tracer l'init du hook overlay.
12. **Inputs souris fuient vers ER à travers F1 + curseur ancré au centre.** Quand F1 est ouvert, ER continue de recevoir les inputs souris (le menu ne capture pas exclusivement) ; en plus ER ré-ancre/force le curseur au milieu de l'écran. Besoin : à l'ouverture de F1, **forcer l'unlock du curseur** et **capturer les inputs souris** pour ImGui (bloquer le passthrough vers ER). Probablement lié au double-draw (point 11) et au desync curseur (point 6). Lié [[input-device-active-flag]].
13. **Minimap ignore le marker-scale ET le clustering.** Le réglage d'échelle des markers et le clustering n'affectent QUE la worldmap, pas la minimap. La minimap devrait honorer les mêmes réglages (ou avoir les siens). Lié [[minimap-future-feature]].
14. **DX minimap : afficher l'objet sélectionné par le searcher sur la minimap.** Quand l'item-search sélectionne une cible, l'afficher sur la minimap avec le même cercle/anneau que sur la worldmap (= "game changer" DX selon <user>). Lié [[overlay-item-search-bar]], [[minimap-future-feature]].
