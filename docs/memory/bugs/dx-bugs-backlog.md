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
2. ✅ **FIXED 2026-07-02** (les deux moitiés). La compensation DX (recentrage curseur souris→manette)
   était faite depuis `4ec2aa7` (PR C). La moitié UI restante — hints de touche auto-switchés selon
   le device actif — est faite (`feat/phase4-device-aware-hints`, **premier vrai run de la boucle
   AI Phase 4** du plan hot-reload) : le close-hint du panel F1 (pill compact + header) affiche le
   combo manette configuré (`mask_to_combo_string(overlayToggleGamepad)`, ex. "Y+R3 close") quand
   `last_input_was_gamepad()`, sinon "F1". Les DEUX branches vérifiées visuellement en jeu via
   hot-reload + screenshot RPC (la branche manette testée en hot-reloadant une version condition
   forcée, puis revert — 3 itérations live, jamais redémarré le jeu). Nit restant (non tracké
   avant, toujours ouvert) : la branche clavier affiche "F1" en dur — si l'utilisateur rebinde
   `overlay_toggle_key`, le hint ment (il faudrait un reverse vk→name).
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
4. ✅ **FIXED 2026-07-02** (`feat/rpc-input-injection`) — pause in-game intégré via la technique
   elden_pause (AOB `PAUSE_BRANCH` dans `re_signatures.hpp`, flip du `je` frame-step 0F 84↔0F 85 =
   pause/resume, le byte EST l'état donc lisible). `src/goblin_pause.{hpp,cpp}` (scan lazy,
   tolérant au byte déjà flippé par un autre patcheur), case F1 "Pause the game" (cachée si la
   sig ne résout pas), RPC `pause 0|1|toggle` + `paused=` dans status. **Validé quantitativement
   en jeu** : monde figé = 0 pixel changé en 3s de screenshots, vivant = ~4.8k ; rendering + F1 +
   RPC restent actifs pendant la pause. NB : `PauseTheGame.dll` (mod externe, MÊME byte patché,
   touches lues via GetAsyncKeyState GLOBAL — peut toggler la pause pendant que le jeu est en
   ARRIÈRE-PLAN quand on tape sa pause-key dans une autre app, cause probable du "bloqué en pause
   unfocused" relevé par <user>) est maintenant redondant → à retirer du profil me3. Item 5
   (auto-pause à l'ouverture F1) reste ouvert — trivial maintenant (un setting + 2 lignes).
   *Original report:* Intégrer le mod "Pause in game" directement dans MapForGoblins — une case en
   plus dans F1. Évite tout conflit de touche possible avec le mod externe. RE reference
   (<user>, 2026-07-01): https://github.com/iArtorias/elden_pause.
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
   **FOLLOWUP 2026-07-02 (<user> question "les NPC ont leurs badges?"): NON — fixed
   (`fix/npc-altitude-badge`).** Une 4e source droppait posY: les quest-NPC pins passent par
   `entity_world_pos`/`g_entity_pos` qui ne stockait que X/Z. `EntityPos` gagne `wy` (rempli depuis
   `DiskEnemy.posY`/`DiskCollectible.posY`), `entity_world_pos` a un out `worldY` optionnel, les 2
   sites de pin (step-following + auto-detected) posent `m.worldY`. Build clean; vérif visuelle
   in-game à faire (badge ▲/▼ sur un pin NPC à altitude ≠ joueur).
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

F3. ✅ **FIXED + user-confirmed live 2026-07-01** (`90b3e2b`) — poll clavier
    (`GetAsyncKeyState`+`ToUnicodeEx`, `src/input/input_keyboard_poll.cpp`) devenu la SEULE
    source de texte clavier pendant que le menu est ouvert ; `input_wndproc.cpp` ne relaie plus
    `WM_CHAR`/`WM_KEYDOWN`/`WM_KEYUP`/`WM_SYSKEYDOWN`/`WM_SYSKEYUP` à ImGui du tout (évite le
    double-frappe pendant que les messages legacy marchaient encore, avant tout Alt+Tab). Limite
    connue : pas d'auto-repeat OS émulé (une touche maintenue tape une fois par appui physique).
    **Clavier définitivement mort dans les search bars ImGui après un Alt+Tab — DÉTERMINISTE,
    repro fiable trouvée par <user> (2026-07-01, après le refactor `feat/input-module`).** Repro
    exacte : (1) écrire dans une search bar F1 avec le clavier → marche ; (2) Alt+Tab away puis
    retour ; (3) réessayer d'écrire → **plus rien ne s'écrit**, à chaque fois. Log
    `MapForGoblins.log` 17:33:17-24 : `wm_char/sec=17` avant l'Alt+Tab (ça tapait) →
    `WM_KILLFOCUS` (19.425) → `WM_SETFOCUS` (20.037, UNE seule rising-edge propre, pas de flap —
    la régression de flapping de l'arc précédent reste bien fixée) → à partir de 20.588,
    `wm_char/sec=0 wm_keydown/sec=0` **pour le reste de la session (4+ échantillons, 4+ secondes)**
    alors que `WantCaptureKeyboard=true WantTextInput=true` tout du long (ImGui pense toujours
    qu'un champ a le focus, mais AUCUN message clavier n'arrive plus jamais à `hk_wndproc` —
    compteur brut, incrémenté avant toute logique de consommation). PAS une régression du refactor
    input-module (le focus-tracking lui-même reste propre, un seul edge) — bug préexistant, juste
    mieux diagnostiqué maintenant grâce à une repro déterministe au lieu d'un cas rare. **Hypothèse
    forte, même famille que le fix clic-souris Proton** (`docs/memory/bugs/overlay-input-hook-freeze.md`
    / changelog "Overlay menu unclickable on Wine/Proton") : ER lit le clavier via raw input
    (`RIDEV_NOLEGACY` probable), et un cycle Alt+Tab semble faire re-basculer ce mode pour le
    clavier spécifiquement (la souris a déjà son fallback poll `GetAsyncKeyState`, pas le clavier).
    **Fix probable : poll clavier (`GetAsyncKeyState` par VK + `ToUnicodeEx` pour la traduction
    caractère/layout/shift) en fallback, même pattern que le clic souris** — mais nécessite
    d'éviter les doublons quand les messages legacy marchent encore (avant tout Alt+Tab). Pas
    encore implémenté — prochain candidat naturel après ce refactor. Compteurs déjà en place
    (`input_wndproc.cpp` `diag_wm_char_exchange`/`diag_wm_keydown_exchange`) pour valider le fix.

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

---

## Review-3 — session manette <user> 2026-07-28 (vmap markers + chips + HUD)

Cinq symptômes rapportés d'un coup, tous ✅ **FIXÉS le 2026-07-28** (`master`, 2 commits). Ce qu'il
faut retenir au-delà des fixes eux-mêmes :

18. ✅ **Le pointeur pad n'est pas le même objet sur les deux cartes — et c'était la racine de
    "sélectionnable mais pas interactable".** Sur la **vmap** le pointeur pad est la réticule stick-droit
    et elle écrase `io.MousePos` → tout ce qui teste un rect en `io.MousePos` HOVER correctement, mais
    tout ce qui teste `IsMouseClicked` reste MORT (aucun bouton pad ne synthétise un clic souris). D'où
    le symptôme "le chip s'allume mais rien ne se passe". **Règle : un hit-test manuel dans un ImDrawList
    doit toujours avoir sa branche d'activation pad explicite** — le hover pad est gratuit, l'activation
    ne l'est jamais. Sur la **carte native** il n'y a AUCUNE réticule libre à la manette : ER fixe le
    réticule au CENTRE DE L'ÉCRAN et fait défiler la carte dessous. Le pointeur effectif passé à
    `render_markers` est donc le centre écran dès `last_input_was_gamepad()` (`goblin_overlay_render.cpp`),
    pas le curseur OS resté garé — sinon le pad ne peut même pas survoler. (Le champ `+0xFC` "réticule"
    de WorldMapArea ne sert à rien ici : il ne suit QUE la souris, cf. [[overlay-gamepad-cursor-bugs]].)
19. ✅ **Le pendant pad de "le chip mange le clic".** `input_wndproc` bouffe déjà le `WM_LBUTTONDOWN`
    quand `call_inworld_hovered()` — mais la manette est POLLÉE, pas messagée, donc rien n'était
    interceptable là. Fait dans `hk_xinput_get_state` (`input/input_gamepad.cpp`) : quand F1 est fermé,
    la carte native ouverte et le réticule sur un chip, on masque **uniquement `XINPUT_GAMEPAD_X`** pour
    les appelants HORS de notre module — ImGui (appelant interne, `caller_is_us`) voit toujours le vrai X
    et fait le toggle. Le hook n'avait qu'un mode "tout zéro pendant `menu_open()`" ; c'est maintenant un
    masque par bouton, le point d'entrée existe pour tout futur conflit binding mod/jeu.
20. ✅ **Ordre de dessin = ordre d'écriture, et il n'y a aucune notion de z-index.** Les pins custom
    étaient dessinés AVANT les labels de région → un pin posé sur "Limgrave" disparaissait sous la
    pastille. Corrigé en remontant le bloc labels. Ordre canonique du canvas vmap, du fond vers le
    dessus : tuiles → relief → markers/clusters → search marks → **labels de région** → pins custom +
    death marker → co-op → curseur joueur. Toute nouvelle couche doit se placer explicitement dedans.
21. ✅ **Un compteur monotone n'est pas un nom.** `s_custom_seq++` ne redescendait jamais : après
    suppressions, le pin suivant s'appelait "Marker 9" à côté de trois entrées. Remplacé par le plus petit
    "Marker N" libre recalculé à l'ajout (24 pins max/groupe, le scan est gratuit). Même piège partout où
    un libellé auto est dérivé d'un compteur plutôt que de l'état.
22. ✅ **Focus nav vs canvas : X tirait des deux côtés.** `pad_place` ne testait que "réticule sur le
    canvas", donc naviguer la sidebar Marker et presser X posait un pin derrière. Gate publique, sans
    ImGui internals : `IsWindowFocused(ChildWindows) && !IsWindowFocused()` = "un enfant (donc une
    sidebar) a le focus, pas la fenêtre elle-même", plus `IsPopupOpen(AnyPopup)` pour le clavier virtuel.
    À réutiliser pour tout futur bouton pad "action de canvas".
23. ✅ **Champ texte sans clavier virtuel = inaccessible au pad.** Tous les champs *filtre* appelaient
    `draw_gamepad_keyboard_button` ; le champ *nom* des markers custom était le seul InputText non-filtre
    et avait été oublié → renommer un pin était impossible à la manette. **Checklist : tout nouvel
    InputText doit câbler l'OSK**, sinon il est mort pour la moitié des joueurs.
24. ✅ **Deux surfaces HUD ancrées chacune dans son coin, aucune ne connaît l'autre.** La minimap
    (`map_renderer.cpp`) et le HUD run (`panel_run.cpp`) calculent leur position depuis leur propre
    `*AnchorRight/Bottom` + offsets ; n'importe quelle paire d'offsets dans le même coin les empile en
    silence, et comme la minimap est sur le `BackgroundDrawList` alors que le HUD est une vraie fenêtre,
    le HUD gagne toujours (la ligne "Deaths … Bosses" barrait le disque et cachait le N de la boussole).
    Fix minimal choisi avec <user> : la minimap publie son rect (`worldmap::minimap_screen_rect`) et le
    HUD run glisse VERTICALEMENT du côté le plus proche de la position demandée. **C'est un patch 1-vs-1,
    pas un système** : à la 3ᵉ surface HUD ancrée (boussole, tracker de quête, coords…) il faudra le
    vrai système de slots (coin + ordre, pile auto) plutôt que N patchs d'évitement croisés — le dire à
    <user> à ce moment-là au lieu d'empiler (cf. plafond de complexité).

25. ✅ **FIXÉ 2026-07-28 — `SetKeyOwner` NE couvre PAS le mode "windowing" d'ImGui.** Symptôme : X
    MAINTENU sur la vmap affichait un fond assombri + la barre de titre surlignée par-dessus la carte
    (screenshot <user>). `ImGuiKey_NavGamepadMenu == ImGuiKey_GamepadFaceLeft`, et on possédait déjà la
    touche — mais `NavUpdateWindowing` la lit via `IsKeyPressed(key, ImGuiInputFlags_None)`, dont le
    `owner_id` vaut par défaut **`ImGuiKeyOwner_Any` = "ignore l'ownership"** (imgui.cpp 1.90.9,
    `NavUpdateWindowing`). Donc **posséder une touche ne suffit pas** : il faut vérifier CHAQUE
    consommateur ImGui de cette touche. Fix : après `NewFrame` (où `NavUpdateWindowing` a déjà tourné),
    on remet `NavWindowingTarget/TargetAnim/HighlightAlpha/ToggleLayer` à zéro **si la source est
    Gamepad** — désarme dans la frame même où ça s'arme (l'alpha ne quitte jamais 0, ni focus-change ni
    toggle-layer au relâchement), et un X maintenu ne peut pas ré-armer (il faut un front de pression).
    CTRL+TAB clavier intact. Même piège pour toute autre touche pad qu'on réquisitionnerait.
26. ✅ **FIXÉ 2026-07-28 — le mode spoiler-free mangeait le badge d'altitude par accident.** La branche
    anonymisée de `draw_marker_impl` fait `return` AVANT le `draw_altitude_badge` que tous les autres
    chemins appellent → activer `anonymous_loot` supprimait aussi la flèche de hauteur. Or la hauteur
    relative au joueur **n'est pas une identité** (elle dit au-dessus/en-dessous, jamais QUOI). Nouveau
    réglage `anonymous_loot_altitude`, **défaut `true`** (le badge survit à l'anonymisation, toujours
    sous le gate `altitude_cue`), `false` = blackout strict. Leçon générale : un `return` anticipé dans
    un chemin de rendu "dégradé" emporte silencieusement TOUT ce qui est en aval — lister ce qu'on perd.
