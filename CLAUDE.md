# MapForGoblins — Claude Code entry point

> [!IMPORTANT]
> **REPRISE DE LA SESSION PRÉCÉDENTE (interrompue par limite de quota) — À SUPPRIMER DANS LA PROCHAINE SESSION UNE FOIS TERMINÉ**
>
> Les conversations de la session précédente ont été récupérées et enregistrées :
> - **Session principale** : [Plan_C_Havok_World_c8e5a25f.md](file:///C:/Users/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL/recovered_conversations/Plan_C_Havok_World_c8e5a25f.md)
>
> ### Résumé des chantiers pour la reprise :
>
> 1. **ERR-MapForGoblins-DLL — Plan C Havok Worlds (Énumération des mondes Havok)**
>    - **Repo / Branche** : [ERR-MapForGoblins-DLL](file:///C:/Users/iamacat/Documents/GitHub/ERR-MapForGoblins-DLL) (branche `master`)
>    - **Statut** : En cours (interrompu en plein vol, collisions non fonctionnelles, passage au Plan C).
>    - **État / Reste à faire** :
>      - Les 7 champs de notre pilier sont patchés (readbacks verts) et champ pour champ, le pilier est un clone d'un body statique du monde (hors shape/position). Re-tester les collisions du pilier.
>      - Si les collisions ne fonctionnent toujours pas, passer au **Plan C** : le character se déplace peut-être dans un autre monde Havok que celui dans lequel nous ajoutons (Elden Ring possède plusieurs mondes Havok comme le monde statique, les ragdolls, etc.). Le down-ray casterait dans notre monde, mais le character serait dans un monde frère.
>      - Dans ce cas, dumper `CSPhysWorld` pour énumérer les `hknpWorld` (la vtable `hknpWorld` est connue à l'adresse : `eldenring.exe + 0x2eedc78`) et ajouter le body dans le bon monde Havok.
>      - Pendant la décompilation Ghidra en arrière-plan, préparer le patch avec des modes paramétrables jusqu'au RPC afin de pouvoir itérer à chaud par la suite.
>

Claude Code auto-loads this file. The full agent handoff lives in `AGENTS.md`, imported below.

@AGENTS.md

## Project memory — single store

- All durable project memory lives in **`docs/memory/`** in this repo and **`docs/changelog.md`**.
- This is the ONLY memory store. Do **not** create or write to a separate per-agent / per-machine
  memory (local agent memory, Serena memories, `~/.claude` memory, imported tar/rar dumps). The old
  Linux + Windows machine memories were merged into this repo on 2026-06-29 and must not diverge again.
- Read `docs/memory/common.md` first, then the relevant `docs/memory/{features,bugs,tooling,process}/README.md`.
- On any completed task that changes durable state, update the right `docs/memory/` file (+ `changelog.md`
  if it adds a feature or fixes a bug) and commit — never stash it in a side memory.

## Overlay hot-reload split — keep BOTH DLLs linking (avoid drift)

The mod builds two ways from the SAME sources: the shipped **single DLL** (`build-linux`, default) and a
DEV **split** (`build-linux-hotreload`, `GOBLIN_OVERLAY_HOTRELOAD=ON`) that puts the draw layer in a
swappable `goblin_overlay_render.dll` so render/marker/panel edits reload into the running game with **no
restart** (watcher auto-swaps ~1.3s, or the `reload_overlay` RPC). Loop + tasks: see `docs/HANDOFF.md`
"OVERLAY HOT-RELOAD RESYNC" + the `.vscode/tasks.json` "Hot-reload …" tasks.

- **The split silently rots** because `build-linux` (single DLL) links every symbol regardless of the
  host↔render boundary, so a new **host→render** call (e.g. a new `debug_rpc` verb calling a `panel::` or
  `worldmap::` function) compiles fine in the default build but leaves the split's host DLL with an
  undefined symbol. It bit twice already (2026-07-05 resync fixed ~40 of them).
- **RULE: if your change adds or moves a call across the host↔render boundary** — host code (dllmain,
  `goblin_debug_rpc.cpp`, `goblin_overlay.cpp`, `goblin_overlay_render_api.cpp`, `goblin_mod.cpp`) calling
  into render code (`overlay_panel/`, `worldmap/map_entry_layer.cpp`/`map_renderer.cpp`/`grace_layer.cpp`,
  `goblin_overlay_render.cpp`), or vice-versa — **before you finish, run:**
  `ninja -C build-linux-hotreload MapForGoblins goblin_overlay_render` and fix any new `undefined symbol`.
  (Pure host-only or render-only edits don't need it.) Fix patterns, in order of preference:
  1. **Stateful data/logic file misfiled in render** → move it to `GOBLIN_HOST_SOURCES` + mark its public
     API `GOBLIN_RENDER_API` (host exports; render imports via `MapForGoblins.lib`). See vworld/maptile.
  2. **A render-resident fn the host must call** (marker/panel/relief code that must STAY hot-reloadable) →
     add a loader export (`MFG_*` in the `map_entry_layer.cpp` extern block + typedef/field/GetProcAddress/
     `call_*` wrapper in both branches of `goblin_overlay_render_loader.cpp`). std::string returns fill a
     caller buffer. For a whole verb family, prefer ONE generic dispatch export (see `MFG_VmapCommand`).
  3. **A render fn calling an unexported host fn** → mark the host fn `GOBLIN_RENDER_API`, or repoint the
     render caller at the existing `overlay_api::` export.
- Keep BOTH builds green at every commit; the default single-DLL build is the shipped/played one.

## VSCode dev tasks (`.vscode/tasks.json`)

- Build/deploy + RPC test runners live here (Terminal ▸ Run Task…). "Run ONE RPC test (pick)" shows a
  dropdown of `tools/rpc_tests/test_*.py`; "Boot ER + RPC repl" and "RPC one-shot (type command)" drive
  a running game (e.g. `hf_probe`, `warp <id>`, `ping`).
- **When you add a new `tools/rpc_tests/test_*.py`, also add its filename to the `rpcTest` pickString
  `options` in `.vscode/tasks.json`** — `run_all.py` auto-discovers tests by glob, but the VSCode picker
  cannot (pickString options must be literal). A comment at that list says the same.
- **Regenerate + commit the git-tracked `tools/rpc_tests/STATUS.md` whenever you run RPC tests.** It is
  auto-generated by `tools/rpc_tests/check_regress.py` from the LOCAL, gitignored ledger
  (`tools/rpc_tests/results.jsonl`), so it advances ONLY when someone regenerates it and commits — left
  alone it silently drifts (a test never run on this box is absent; an old run shows a stale date). If you
  ran any test this session (one `test_*.py` or the `run_all.py` sweep), before you finish run
  `python tools/rpc_tests/check_regress.py` and commit the updated `STATUS.md` alongside your change, so
  the versioned PASS/FAIL snapshot reflects the current dev box. Never hand-edit it. (`run_all.py` already
  regenerates it at the end of a sweep — the gap is that it is rarely run + committed, not that the tool
  is missing.)

<!-- auto-optimizations -->
## 🔧 Auto-Optimizations (generated by context_analyzer)

- **Session 2026-07-08 06:30** — 4.7 MB, 284 turns
  - ⚠️ Re-lectures : `goblin_debug_rpc.cpp` x4, `HANDOFF.md` x2, `goblin_w2s.cpp` x2, `goblin_warp.hpp` x2 → Utiliser des line ranges ciblés
  - ⚠️ 17 outputs > 10 KB (3.4 Mo total) → pipe dans head/tail ou filtrer avec rtk
  - ⚠️ 2/150 cmds sans `rtk` (1%) → Toujours préfixer `rtk`
- **Session 2026-07-08 16:33** — 9.2 MB, 38% tool outputs, 630 turns
  - ⚠️ 13 commandes stdout > 10 KB (3293 KB) → `| head -n 50` ou `rtk`
  - ℹ️ rtk utilisé sur 99% des commandes éligibles (2 manqués)

<!-- /auto-optimizations -->

