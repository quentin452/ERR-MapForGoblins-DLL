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
2. 🟡 **PARTIEL — FIXED 2026-07-01** (`4ec2aa7`, PR C — code comment cite "Item 2") — la compensation
   côté DX (recentrage du curseur au milieu quand on passe souris→manette) est faite. Doc jamais
   croisée à l'époque, trouvé en auditant les commits vs le backlog. **Reste ouvert :**
   auto-switcher les hints de touche (manette/souris) selon le device actif — la donnée existe déjà
   (`g_last_input_was_gamepad`, PR C) mais l'UI hint-icon elle-même n'a jamais été codée (noté comme
   follow-on "cheap" dans `docs/plans/dx_bugs_backlog_plan.md`, pas fait).
   **Bug ER natif manette (DX upstream)** — quand on déplace le curseur à la souris puis qu'on repasse à la manette, ER devrait recentrer le curseur au milieu mais ne le fait pas. (Bug du jeu, pas le nôtre — DX à compenser éventuellement.) ➕ <user> veut aussi auto-switcher les hints de touche (manette/souris) selon le device actif → LIRE le flag "active input device" d'ER, cf. [[input-device-active-flag]] (= MÊME flag que le drift worldmap-manette ; recette CE memory-diff dans le brief RE).
3. 🟡 **PARTIEL — FIXED** (PR C `feat/gamepad-toggle-cursor-recenter` 2026-07-01, PR C-2 part 1
   `feat/gamepad-nav-input-isolation` 2026-07-01) — combo XInput configurable (défaut `Y+R3`)
   ouvre/ferme F1 (PR C), ET navigation complète des widgets (boutons/checkboxes/listes) via D-pad/
   stick + A/B, avec isolation d'input (le jeu ne reçoit RIEN de la manette tant que F1 est ouvert —
   hook `XInputGetState`, voir PR C-2 part 1 dans `docs/plans/dx_bugs_backlog_plan.md`). Recorder en
   jeu pour changer le combo, avec garde anti-lockout (rejette un combo à 1 seul bouton nav A/B/X/Y/
   D-pad). Vérifié en jeu 2026-07-01. **Reste ouvert :** **taper dans la search bar** nécessite
   toujours clavier (ImGui nav ne gère pas le texte libre) — donc "jouer end-to-end uniquement à la
   manette" presque atteint, sauf la recherche texte. Followup tracké : PR C-2 part 2 dans
   `docs/plans/dx_bugs_backlog_plan.md`. Original report ci-dessous.

   ✅ **PR C-2 part 2 DONE 2026-07-01** (`feat/gamepad-virtual-keyboard`) — clavier virtuel à
   l'écran (popup boutons, réutilise la nav ImGui existante) pour les 3 champs texte (recherche
   item, filtre catégorie, filtre NPC quête), layout Alphabetical/QWERTY au choix. **"Jouer
   end-to-end à la manette" est maintenant complet.** 2 bugs annexes trouvés + fixés pendant la
   vérif : bouton "Kbd" invisible (placé hors panel par un `SameLine()` après un champ 100%
   largeur) ; et un bug PLUS SÉRIEUX hérité de PR C-2 part 1 — la souris se retrouvait totalement
   bloquée après ouverture F1 à la manette (boucle de feedback : notre `SetCursorPos` de recentrage
   générait un `WM_MOUSEMOVE` que notre propre code prenait pour un vrai mouvement, ce qui
   réarmait le recentrage en continu tant que la manette restait "active" — curseur épinglé au
   centre chaque frame). Détail dans `docs/plans/dx_bugs_backlog_plan.md` PR C-2 part 2.

   ✅ **FIXED + log-confirmé 2026-07-01** (branche `fix/gamepad-input-flag-debounce`, pas encore
   mergée) — followup relevé <user> même session, après PR C-2 part 2 : **3 bugs distincts**,
   tous confirmés par preuve de log (`[FOCUSDIAG]`/`[KBDIAG]`, ajoutés cette session), pas par
   déduction seule. Chronologie de l'investigation :
   1. **1er essai (partiel/faux) :** `g_last_input_was_gamepad` n'avait aucun gate `fg` sur son
      écriture + aucun debounce sur l'edge mouse→pad, donc une manette légèrement active pouvait
      re-armer `recenter_cursor_to_window()` quasi à chaque frame → snapait le curseur au centre
      à chaque interaction souris, cassant le scroll/survol des panels de recherche. Fix :
      `fg`-gate + debounce `kGamepadSwitchDebounceFrames` (5 frames) + reset sur `WM_KILLFOCUS`
      et sur tout vrai message souris/clavier. **Vrai fix pour un vrai bug**, mais <user> a
      re-testé et l'Alt+Tab cassait TOUJOURS l'input — donc PAS la cause du bug Alt+Tab.
   2. **Root cause Alt+Tab, trouvée via log :** `[FOCUSDIAG]` a montré qu'un seul VRAI cycle
      focus (1× `WM_KILLFOCUS` puis 1× `WM_SETFOCUS`) produisait **7 rising-edges de `g_show`**
      en ~20s sans aucun autre changement de focus réel. Cause : `fg` était re-CALCULÉ chaque
      frame present via `GetForegroundWindow() == g_hwnd` (poll) — sous Wine, cet appel renvoie
      transitoirement autre chose que `g_hwnd` pendant quelques frames lors de la transition
      compositor de l'Alt+Tab, donc le poll captait ces états intermédiaires. Chaque flap
      fermait/rouvrait la fenêtre ImGui (draw gated sur `g_show`), réinitialisant tout l'état
      hover/focus interne à chaque fois — rien n'avait de frame stable pour enregistrer un
      clic/scroll/frappe. Fix : nouveau `std::atomic<bool> g_has_focus`, mis à jour UNIQUEMENT
      par `WM_SETFOCUS`/`WM_KILLFOCUS` (event-driven, ne se déclenche que sur de vrais
      changements) ; `fg` (`hk_present`) et `fgw` (poll redondant dans le bloc clic-souris
      Proton) lisent maintenant `g_has_focus` au lieu de re-poller `GetForegroundWindow()`.
   3. **2e bug distinct, trouvé via log (<user> : "même sans unfocus/refocus, le clavier peut
      perdre le hooking 'searching'") :** `[KBDIAG]` a montré la MÊME signature (`g_show`
      rising-edges répétés) mais **sans aucun `WM_SETFOCUS`/`WM_KILLFOCUS` entre eux** — donc
      `g_user_show` (le toggle lui-même) flappait, pas `fg`. Cause : le check du combo manette
      de toggle (`combo_down && !g_prev_gamepad_toggle_down`) n'avait AUCUN debounce — un
      comportement XInput connu (les premières lectures juste après qu'une app reprenne le
      focus peuvent être une rafale stale/glitchée) pouvait faire bouncer la lecture plusieurs
      fois, chaque bounce fermant/rouvrant le panel et cassant le focus clavier de l'InputText
      de recherche. Fix : debounce `kToggleGamepadDebounceFrames` (3 frames consécutives) avant
      de committer le toggle, armé une seule fois par appui (ré-armé au relâchement). Supprimé
      `g_prev_gamepad_toggle_down` (devenu mort).
   - **Confirmation intermédiaire (log, <user> 2026-07-01) :** un vrai Alt+Tab ne produit plus
     qu'UNE seule rising-edge de `g_show` (corrélée au `WM_SETFOCUS`), avec `wm_keydown` de
     nouveau non-nul en ~2s (vs 15+ secondes bloqué à 0 avant le fix) ; les autres rising-edges du
     log (ouvertures/fermetures volontaires F1) restent isolées, plus de bounce répété. Mais
     <user> a retesté et **le clic/curseur restait cassé après Alt+Tab** — ce fix réglait le
     flapping mais pas tout le bug.
   3. **Root cause du "can't click" restant, trouvée via `[KBDIAG]`** (nouveau log ajouté cette
      session) : ImGui_ImplWin32 ne feed `io.MousePos` que via `WM_MOUSEMOVE`, que le jeu
      supprime pendant le gameplay normal (raw input) — même raison que le clic gauche est déjà
      pollé (`GetAsyncKeyState`) plutôt que lu depuis `WM_LBUTTONDOWN`. `WM_KILLFOCUS` invalide
      `io.MousePos` et rien ne le rafraîchit plus jamais après → log confirmé : `MousePos` bloqué
      au sentinel invalide (-FLT_MAX) 26+ secondes d'affilée, `WantCaptureMouse` toujours faux
      même si le poll du bouton voyait bien de vrais clics. 1er fix : poller `GetCursorPos` +
      `ScreenToClient` → `io.AddMousePosEvent` chaque frame, même pattern que le poll du bouton.
   4. **Régression du fix #3, trouvée par <user> en jeu :** le curseur se mettait à snapper/rester
      collé au CENTRE de l'écran instantanément à l'ouverture de F1. Cause : ER garde le curseur
      OS réellement warpé au centre en continu pendant le gameplay normal (caméra raw-input, même
      comportement que documente déjà le commentaire "swallow the game's recenter-to-middle" de
      `hk_set_cursor_pos`) — donc le tout premier poll après ouverture lit VRAIMENT le centre, et
      feed cette valeur périmée à ImGui donnait l'impression d'un curseur recentré/collé. Tentative
      de fix (baseline : ne feed la position qu'après un premier mouvement réel détecté) jugée
      insuffisante par <user> (le recentrage persistait).
   5. ✅ **FIX FINAL, confirmé par <user> en jeu 2026-07-01 :** `g_show` (qui pilote le draw ET
      TOUS les hooks de capture input) ne dépend plus de `fg` (focus OS) du tout — seulement de
      `g_user_show` (le toggle F1 lui-même). Supprime la transition de focus elle-même au lieu de
      corriger chaque bug qu'elle produisait (MousePos invalide, WantCaptureMouse qui ne revient
      jamais, curseur collé au centre) — root-fix de toute la classe de bug, pas un patch de plus.
      **Tradeoff accepté :** F1 reste actif (y compris le swallow input) même si le jeu perd le
      focus — si l'utilisateur alt-tab vers une AUTRE fenêtre avec F1 encore ouvert, nos hooks
      peuvent interférer avec cette fenêtre ; fermer F1 avant dans ce cas.

   **Bug DX chez nous — F1 inaccessible à la manette** — impossible d'ouvrir les menus via F1 (équivalent manette). Donc impossible de jouer MapForGoblins end-to-end uniquement à la manette. → besoin d'un binding manette pour l'ouverture du menu.
4. **Intégrer le mod "Pause in game" directement dans MapForGoblins** — une case en plus dans F1. Évite tout conflit de touche possible avec le mod externe.
   **RE reference trouvée (<user>, 2026-07-01): https://github.com/iArtorias/elden_pause** —
   implémentation externe existante d'un pause-in-game pour Elden Ring (AOB/hook approach à
   étudier avant de coder le RE spike nous-mêmes; peut raccourcir/remplacer le spike RE demandé).
5. **Setting ImGui "pause à l'ouverture de F1"** — quand on ouvre F1 (manette ou clavier), proposer un setting pour choisir si le jeu se met en pause automatiquement à l'ouverture des settings MapForGoblins. Hypothèse DX : quand le menu F1 est ouvert on ne veut pas forcément jouer en même temps → peut-être meilleure DX qu'une simple case ImGui à long terme. Lié au point 4.
6. ✅ **FIXED 2026-07-01** (`4ec2aa7`, PR C — code comment cite "Item 6") — recentrage du curseur
   sur la transition (re)open de la worldmap, via `o_set_cursor_pos` déjà hooké. Doc jamais croisée
   à l'époque, trouvé en auditant les commits vs le backlog.
   **Desync curseur ImGui ↔ ER au restart de la map** — quand on restart la map, le curseur devrait être recentré au milieu pour que les ToolTips s'affichent au même endroit que le curseur ER. Lié à [[overlay-item-search-bar]] (cursor-locate = caméra map 2D).
7. ✅ **FIXED 2026-06-30** (`feat/dx-altitude-cue`, PR B) — ▲/▼ triangle badge (AddTriangleFilled, no
   font dep) when a marker's Y differs from the player's, gated to the player's current map layer
   (group match) so cross-map viewing doesn't badge everything. MSB block-local Y (`pos[1]`) threaded
   onto markers (added posY to DiskTreasure/DiskEnemy + captured in the parser). F1 toggle + ini
   `altitude_cue`/`altitude_deadzone`. Validated in-game (direction tracks player altitude). FOLLOW-UP:
   3 niche sources still drop posY (ReforgedRune/EmberPieces `bpos`, scarab/maps-pass `pos`, sibling-LOD
   `lit`) → those markers get no badge; thread if needed.
   **DX pour icône plus haut/bas que le joueur (axe Y)** — trouver une indication visuelle quand une icône sur la map est à un Y différent du joueur, pour que le joueur ne cherche pas au mauvais étage/altitude.
   **FOLLOWUP 2026-07-01, FIXED** (<user>: badge only updated on worldmap open, frozen on minimap) —
   `g_player_world_y`/`g_player_group` (`map_renderer.cpp:330-332`) were only refreshed inside
   `render_markers()`, which only runs while the worldmap is OPEN; `draw_minimap()` (map closed)
   reads those same statics via `draw_altitude_badge()` but never refreshed them itself, so the
   minimap badge froze at whatever Y was cached on last map-close. Fixed by sampling
   `get_player_world_pos()` live inside `draw_minimap()` too, every frame it draws. Audited other
   per-marker attributes for the same "cached, never refreshed" pattern while investigating — no
   wider issue found: live-state gates (collected/graying, story/fragment flags, search-hit, region
   toggles) all already re-read every frame; only spatial/structural stuff (clustering, stacking,
   `ref_grace_y`) is build-cached by design (correct, since it's static geometry). Player Y was the
   one true outlier. **User-confirmed fixed live in-game (2026-07-01).**
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

F1. ✅ **NOT REPRODUCIBLE — closed 2026-07-01** (<user> re-tested, confirmed false bug). **Fuite
    d'icônes NATIVES overworld → underground après téléport Browser** (report original ci-dessous).
    Plus de repro en jeu — pas un bug réel (ou déjà auto-résolu par des changements ultérieurs). Pas
    de code touché.
    *Original report:* En se téléportant via le Browser DLC → underground, on voit les icônes
    natives ER de l'overworld (grottes/églises/ruines/donjons mineurs) S'AFFICHER dans la page
    underground. = ce sont les icônes du JEU (natives), pas nos markers ImGui (qui eux respectent le
    `group` = isDLC*2|isUG). Probablement un desync de page-state ER au téléport Browser. Lié
    [[category-icons-00solo-atlas]] (migration GPU/native icons), [[session-2026-06-23-map-icons]]
    (suppression draw-only des natives).

F2. **Locate/recherche ne recentre pas sur une cible dans le Fog of War.** Quand la recherche (cursor-locate) téléporte vers un objet situé dans le fog of war, le **pan s'arrête au bord de la zone de pan visible** et ne recentre PAS sur l'item cible (l'item reste hors-vue / en bord de carte). = le pan est **clampé aux bornes visibles** (panX/panZ bornés), donc une cible au-delà de cette limite (zone non explorée / fog) ne peut pas être centrée. Piste <user> : **bypasser la limite de pan / ajouter le support du PAN OOB (out-of-bounds)** pour autoriser le recentrage au-delà des bornes explorées. À regarder plus tard. Lié [[overlay-item-search-bar]] (cursor-locate / take_locate_pos = caméra map 2D), [[worldmap-unsearched-fog-mask]] (oracle fog), et le clamp de pan dans la projection (project_screen / panX-panZ, goblin_projection.hpp + map_renderer.cpp).

    **Re-relevé <user> 2026-07-01** ("pan lookup blocked to the current Player tile instead of
    the worldXZ tile on Item research lookup" — <user> lui-même : "bug already tracked").
    Vérifié ce jour côté code (lecture seule, pas de fix) : `take_locate_pos`/`loc_best` dans
    `map_renderer.cpp` (~L1525-1650) prennent déjà les coordonnées projetées DE LA MARKER
    CIBLE (`gU, gV`), pas celles du joueur — donc pas une régression "mauvaise tuile source".
    Le symptôme observé (pan qui semble bloqué / ne recentre pas sur la cible) colle avec ce
    F2 (clamp aux bornes de pan visibles), pas avec un nouveau bug de lookup. Pas de nouvel
    item ouvert — reste F2.

---

## Review-2 — relevé <user> le 2026-06-29 (à traiter plus tard)

10. ✅ **FIXED** (avant même ce report du 2026-06-29 — `fix(fragment-gate): fill interior MapList
    gaps...` + 2 commits liés, 2026-06-27/29) — déjà reflété comme "resolved" dans
    `docs/memory/bugs/README.md` mais jamais croisé ici. **Heuristique visibilité RequireFragment +
    Region perd des icônes.** En cliquant sur une région (Region-click → affichage des objets de
    cette région) ET avec `require_map_fragments`, l'heuristique actuelle laisse certaines icônes
    hors de la plage de visibilité Region et hors de la plage RequireFragment → des MapForGoblins
    markers qui devraient s'afficher ne s'affichent pas. Lié [[fragment-gate-maplist-gap]],
    [[worldmap-tile-fog-re]].
11. 🟡 **Root-caused (HANDOFF, DOUBLE-LOAD de deux variantes DLL) — PAS un bug de code.** Le
    double-draw = artefact du chargement simultané de `MapForGoblins.dll` ET
    `MapForGoblins_vanilla.dll`. Fix pratique immédiat = ne déployer qu'UN seul DLL. Le garde-fou
    runtime (mutex nommé, bail propre + bannière d'erreur si 2e instance détectée) reste un TODO
    non codé — voir la section "Known bugs" de `docs/HANDOFF.md`.
    **Double-draw non-déterministe (F1 + minimap).** Parfois F1 ouvre DEUX fenêtres MapForGoblins : une déplaçable + une figée qui réplique le rendu (XXX fps mais bloquée). La minimap a aussi un double-draw similaire. Lié au point 12.
12. ✅ **FIXED** (`b10e50e` "fully block game input while the menu is open" + `2854600` "free the
    cursor while the menu is open", tous deux **2026-06-18 — antérieurs au report du 2026-06-28/29**,
    donc <user> a probablement reporté contre un build pas à jour. Trouvé en auditant les commits vs
    le backlog. **Inputs souris fuient vers ER à travers F1 + curseur ancré au centre.** Quand F1 est ouvert, ER continue de recevoir les inputs souris (le menu ne capture pas exclusivement) ; en plus ER ré-ancre/force le curseur au milieu de l'écran. Lié [[input-device-active-flag]].
13. ✅ **FIXED 2026-07-01** (branche `feat/minimap-scale-cluster-search`, MERGÉE ; réglages minimap
    confirmés persistants en jeu) — `draw_minimap` (`map_renderer.cpp`) honore maintenant `overlayMasterScale`/
    `overlayIconScale` (était un `half=6.0f` codé en dur) et fait un clustering léger propre à la
    minimap (bucket par cellule écran fixe `kCellPx=14px`, pas le `draw_clusters` du worldmap —
    celui-ci est couplé au hover/tooltips/zoom-adaptatif qui n'a pas de sens sur un HUD de rayon
    fixe). **Minimap ignore le marker-scale ET le clustering.** Le réglage d'échelle des markers et le clustering n'affectent QUE la worldmap, pas la minimap. Lié [[minimap-future-feature]].
14. ✅ **FIXED 2026-07-01** (même branche) — même boucle : dessine l'anneau jaune
    `IM_COL32(255,226,40,255)` autour de tout marker/pile qui contient un `search_hit()`, même
    style visuel que le worldmap. **DX minimap : afficher l'objet sélectionné par le searcher sur la minimap.** Lié [[overlay-item-search-bar]], [[minimap-future-feature]].
15. ✅ **FIXED** (`62eb9a9` "per-lot item count in tooltip" — lit maintenant les 8 slots
    `ItemLotParam` + `lotItemNum01..08` via `goblin::lot_item_count`, per le plan). Doc marquée
    DEFERRED n'avait jamais été mise à jour ; trouvé en auditant les commits vs le backlog.
    **Loot undercount + pas de ×N stacking** (détecté 2026-06-30) — "Below The Well" montre 1 Sliver of
    Meat alors que Mapgenie en liste 3. Plan détaillé : `docs/plans/loot_item_count_plan.md`.

16. **Bug ER natif — zoom/dézoom stick droit parfois impossible** (relevé <user> 2026-07-01, pendant les
    tests PR C-2). Parfois le zoom/dézoom de la caméra worldmap via le stick droit de la manette cesse de
    répondre en jeu (bug natif ER, pas encore RE). Hypothèse <user> : hooker le zoom stick-droit
    nous-mêmes pour contourner/fixer, similaire à l'item 2 (ER ne recentre pas le curseur nativement à la
    manette — même famille de "bug ER qu'on compense depuis notre hook XInputGetState", PR C-2 part 1).
    Pas encore investigué : repro exacte inconnue (systématique ? lié à un état du jeu — carte
    ouverte/fermée, focus perdu ? lié au double-poll XInput maintenant que PR C-2 hook la fonction ?).
    Vu qu'on hook déjà `XInputGetState` (`hk_xinput_get_state`, PR C-2 part 1), on a déjà le point
    d'interception nécessaire si la fix passe par là — mais il faut D'ABORD confirmer la repro et la
    cause avant de coder quoi que ce soit (pourrait être un bug ER pur, sans rapport avec notre hook, ou
    au contraire une régression introduite PAR notre hook — à vérifier en premier : reproduit-il aussi
    SANS le hook XInput, càd sur une build d'avant PR C-2 ?).

17. ✅ **DONE 2026-07-01** — <user> : "manque de section research pour trouver les features/flags
    dans imgui (beaucoup de settings à chercher)". Écrit `docs/re/imgui_config_flags_research.md` :
    checklist des `IO.ConfigFlags`/settings ImGui pertinents pour la coexistence manette+souris+
    clavier (liés aux items 2/3/6/12/F2 ci-dessus), chacun avec une note "pourquoi ça pourrait
    aider ici". Doc de référence, pas encore de flag testé en jeu — à cocher au fur et à mesure.
