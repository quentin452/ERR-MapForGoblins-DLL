# AUDIT — Capacités runtime actuelles vs. "modder ER complètement à chaud sans regulation.bin"

Audit 2026-07-02 (agent parallèle, ancres file:line vérifiées sur le code checked-out).
Référence de comparaison : `docs/runtime_modding_framework_vision.md`.

## TL;DR

MapForGoblins est aujourd'hui, à 95 %, un moteur de **LECTURE + OVERLAY**. Il lit à chaud tout ce
dont il a besoin (params live, FMG natif, MSB/EMEVD/BHD off-disk, textures GPU résidentes) et
dessine **dans son propre overlay ImGui** — il ne modifie quasiment pas l'état du jeu. Les seules
**écritures** réellement prouvées in-game sont étroites et ciblées : expansion de 2 tables en RAM
(TutorialParam + FMG), flips `areaNo=99` sur des lignes WMPP vivantes (suppression de pins), et
l'écriture d'event-flags (`SetEventFlag`, read-back 5/5).

Point d'architecture crucial : **l'ancienne injection de lignes dans la table native
`WorldMapPointParam` (`inject_map_entries`) n'a plus d'implémentation** — retirée au profit du
rendu overlay (`goblin_overlay.cpp:1563`). Le vrai précédent d'ajout-de-rows-avec-resize-de-table
qui tourne encore est **TutorialParam** (`goblin_tutorial_popup.cpp`).

## 1. Capacités ACTUELLES par domaine

Maturité : **[P]** prouvé in-game · **[◐]** partiel · **[○]** probe/RE seulement.

### Params — LECTURE
| Capacité | Fichier:ligne | Mat. |
|---|---|---|
| Lecture typée de n'importe quelle table param résidente (`get_param<T>`, binary-search + fallback out-of-order) | `from/params.hpp:219,154` | P |
| Offsets de champs auto-résolus (AOB → disp ModRM) : goodsType, sortGroupId, AEG pickup lot, repick-bit, bonfire textId | `re_signatures.hpp:81-117` | P |
| Tables lues en prod : WMPP, ItemLotParam_map/_enemy, NpcParam, EquipParamGoods, BonfireWarpParam, AssetEnvGeometryParam, ActionButtonParam, ObjActParam | `map_entry_layer.cpp`, `loot_disk.cpp` | P |

### Params — ÉCRITURE / INJECTION
| Capacité | Fichier:ligne | Mat. |
|---|---|---|
| Écriture de champs sur ligne vivante : flip `areaNo`/`areaNo_forDistViewMark = 99` | `goblin_section_visibility.cpp:507-546` | P |
| Réécriture ciblée (OR-flag pairs `clearedEventFlagId`/`textDisableFlagId`) sur lignes injectées | `goblin_inject.cpp:343-362` | P |
| **Expansion de table + ajout de rows** (HeapAlloc table plus grande, recopie + templates, relayout locators + wrapper align-16, swap file_ptr/size) — **TutorialParam** | `goblin_tutorial_popup.cpp:135-332` | P |
| Injection de rows WMPP native (`inject_map_entries`) | façade morte, sans implémentation | ✗ |

### FMG / messages
| Capacité | Fichier:ligne | Mat. |
|---|---|---|
| Hook `MsgRepositoryImp::GetMessage` — résout toute string FMG de l'install active, DLC-mergée | `goblin_messages.cpp:423,638` | P |
| **Injection/expansion FMG en RAM** (rebuild FMG v2 + swap slot) — PlaceName (19) + TutorialBody (208) | `goblin_messages.cpp:138-396,482,524` | P |
| Toggle du FMG injecté (swap pointeur vanilla↔expanded) | `goblin_messages.cpp:586` | P |
| Lecture FMG anglais off-disk (msgbnd.dcx, loose-mod-wins) | `worldmap/name_fmg_en.cpp:185,232` | P |

### Disque no-bake (tout LECTURE SEULE — zéro write/repack)
| Capacité | Fichier:ligne | Mat. |
|---|---|---|
| Fichier arbitraire par vpath depuis Data*.bhd/bdt (RSA-2048, path-hash, BHD5, AES range) | `dvdbnd_reader.cpp:375,159,306,250,210` | P |
| Décompression in-process Oodle/KRAK + zlib (loose→packed) | `loot_disk.cpp:213` | P |
| Parse MSB : Treasure/ObjAct events, Asset/Enemy parts, POINT regions, entity binding, GameEditionDisable | `msbe_parser.cpp:138-405` | P |
| Parse EMEVD : ~13 templates award, boss event-1200, portails 90005605, gestures, paintings, quest-NPCs 90005702, lever-lifts | `msbe_parser.cpp:478-793` | P |

### Textures / GFX / GPU
| Capacité | Fichier:ligne | Mat. |
|---|---|---|
| Hook `CSScaleformImageCreator::CreateImage` → sheets + rects sprites | `goblin_icon_harvest.cpp:45-50,280-283` | P |
| Lecture textures GPU résidentes (ID3D12Resource) + repo FD4 (arbre RB) ; parse SB_MapCursor | `goblin_icon_harvest.cpp:391-445,626-752,1727+` | P |
| Création textures + SRV D3D12 à chaud, copies GPU→GPU sous-rects | `goblin_overlay.cpp:250,337,391,625,723` | P |
| Injection texture dans le rendu du JEU | inexistant — overlay seulement | ✗ |

### Event flags
| Capacité | Fichier:ligne | Mat. |
|---|---|---|
| Lecture `IsEventFlag` | `goblin_inject.cpp:274-294` | P |
| **Écriture** `SetEventFlag` (read-back 5/5, UI derrière cheat `questAllowFlagWrite`) | `goblin_markers.cpp:133-142` | ◐ |
| Hook observateur `SetEventFlag` (read-only) | `goblin_debug_events.cpp:206,443` | P |
| Lecture GeomFlagSaveDataManager (collecté) | `goblin_collected.cpp:158` | P |

### Hooks / infra RE
AOB scan + `resolve_field_offset` + health 29 SIG (`re_signatures.hpp:288`) ; MinHook + hook manuel ;
hook DX12 Present/Resize/ExecuteCommandLists ; hook observateur `AddItemFunc` (AOB de la fn de grant
RÉSOLU, usage read-only — `re_signatures.hpp:46`) ; projection native + legacy-conv fold ; sondes
`[PARAMSCAN]/[EMEVDSCAN]/[ABPTEXT]/[ASSETRADAR]` (`goblin_param_scan.cpp`). Tout **[P]**.

### UI
Overlay ImGui complet, toasts codex (rows TutorialParam+FMG injectées), suppression pins natifs +
grace (draw-only hook), census, clustering. **[P]**

## 2. Ce que demande le doc vision

Statut : note de vision, PAS un plan. Ambitions :
- Mod runtime complet (items/boss/contenu) **sans regulation.bin** ni archives repackées — greffe en
  mémoire après le chargement.
- Forme cible : un dossier = 1 DLL + config + assets bruts ; désinstall = suppr. dossier ;
  **composable** (pas de guerre de merge regulation.bin).
- Ajout d'items = machinerie row-inject pointée sur EquipParamGoods/Weapon.
- Contraintes validées : (a) **persistance save** (.sl2 garde les IDs custom → contrat "DLL présent
  au load" + plages d'IDs hautes réservées) ; (b) **modèles/anims = mur dur** (voie réaliste = hook
  de résolution de fichiers servant un FLVER/BND valide) ; (c) croissance de tables param : row-copy
  prouvé, mass-add non prouvé.
- **Ne PAS extraire un framework core avant un 2e mod réel** (le boundary Slice C = future API).

## 3. GAP ANALYSIS

| Gap | Difficulté | Risque | Précédent prouvé le plus proche |
|---|---|---|---|
| A. Write champ param générique | faible | faible | landmark suppression + resolve_field_offset |
| D. Write/inject FMG tout slot | faible | faible | setup_messages (slot-agnostique déjà) |
| G. SpEffect/Bullet edit à chaud | faible-moy | moyen | A+B |
| B. Hot-add rows toute table (resize) | moyenne | moy-élevé | inject_tutorial_popup_rows |
| E. Scripting events (via hooks C++) | élevée | élevé | SetEventFlag write + observers AddItem/SetEventFlag |
| C. Items obtenables bout-en-bout | moy-élevée | élevé (save) | AddItemFunc + FMG + B |
| H. Save-compat (.sl2) | moy-élevée | élevé | aucun (GeomFlag read-only) |
| F. Spawn assets / FLVER custom | très élevée | très élevé | force_load_file + dvdbnd_reader |

Détails :
- **A** : quasi acquis ; manque un helper `param_set_field(param, rowId, offset, value)`.
- **B** : la mécanique TutorialParam existe et tourne ; manque paramétrage stride/paramdef/alignement
  par table + template. Piège : tables id-looked-up exigent le wrapper align-16 ; mass-add non prouvé.
- **C** : row EquipParam (B) + FMG name/desc (D) + icône (F pour du custom) + grant (`AddItemFunc`
  ou ItemLotParam). Bloqué en amont par H.
- **E** : pas de VM EMEVD ; substitut réaliste = orchestrer les primitifs existants (write flags +
  observers) en petite API réactive.
- **F** : le mur dur. Voie = hook de résolution de fichiers côté chargement du jeu redirigeant une
  requête vers notre buffer (dvdbnd_reader sait déjà lire/décompresser). Writer FLVER = sous-projet.
- **H** : contrainte TRANSVERSALE — figer la politique d'IDs réservés AVANT d'expédier quoi que ce
  soit qui écrit dans l'inventaire/flags persistés.

## 4. Ordre de bataille recommandé

**MAJ 2026-07-03 — quick wins A + D FAITS + vérifiés in-game (ERR/Proton).**
1. ✅ **Quick win D — FAIT** : `goblin::inject_fmg_entries(slot, entries)` générique
   (`goblin_messages.{hpp,cpp}`, généralise `patch_fmg_in_memory`) + RPC `fmg_set`. Vérifié :
   inject+override+read-back sur slots base 10 GoodsName / 19 PlaceName. Save-safe (FMG rechargé du
   msgbnd au boot). Commit `ad5e9f5`.
2. ✅ **Quick win A — FAIT** : `goblin::paramedit::param_set_field` / `param_set_field_by_name`
   (`goblin_param_edit.{hpp,cpp}`, offset live via `resolve_field_offset`) + le loader
   `param_overrides.ini` (Slice 3, shippable, `docs/plans/param_override_loader_plan.md`). Vérifié
   in-game. C'est le premier "mod sans regulation.bin".
3. ✅ **Pièce maîtresse B — FAITE + vérifiée in-game** : `goblin::paramedit::param_add_rows` /
   `param_clone_row` (`goblin_param_edit.{hpp,cpp}`, généralise l'expansion TutorialParam ;
   wrapper id→index align-16 systématique ; stride+type lus de la table live ; collision-check Gap H).
   Vérifié sur EquipParamGoods (id-looked-up) : 2 rows ajoutées, données copiées, jeu SURVIT aux
   lookups goods live. RPC `param_clone`. Commit `623e66f`. Le "mass-add non prouvé" est LEVÉ.
4. **C (item bout-en-bout)** : APRÈS H — **H est FIGÉ 2026-07-03**
   (`docs/memory/process/reserved-id-and-load-contract.md` : bandes d'IDs réservées + collision-check +
   contrat DLL-au-load ; bandes numériques à finaliser au démarrage de C via un survey live) ; prototyper
   sur UN item.
5. **E (couche events par hooks)** : petite API réactive sur les primitifs existants.
6. **Long terme F** : hook de résolution de fichiers (FLVER/BND/TPF depuis le dossier mod) — projet
   séparé.

Discipline : pas d'extraction de framework avant un 2e mod ; test mod-agnostique systématique
("résultat correct sur un AUTRE mod ?").
