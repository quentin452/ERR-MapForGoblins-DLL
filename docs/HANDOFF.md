# HANDOFF — live work queue

Living cross-session queue of in-progress / not-yet-finished work. Update at the end of each session.
Committed code + `docs/changelog.md` are the record of DONE; this file tracks WHAT'S NEXT and WHY.

**Housekeeping (2026-07-03, done):** file had grown to 1254 lines, mostly narrative for work already
merged, changelog'd, and in-game verified. Compacted to genuinely live/in-progress work, open
questions, and standing knowledge (gotchas, deferred decisions, non-obvious facts) not captured
elsewhere. History for anything not below: `docs/changelog.md` first, then `docs/plans/*.md`,
then `docs/re/*.md` (RE findings) and `docs/memory/`.

## ⇒ RESUME HERE — sidecar Phase 2 (clean-save item strip/reinject): bracket is LIVE, cap-oracle E2E is next

**Where things stand:** the whole-slot save serialize is found and pinned — `SERIALIZE_FN`
@ `er+0x67dc00` (`FUN_14067dc00`, GameDataMan-xref ∩ DLOutputStream-writer, save-specific/synchronous/
direct-called; full RE in `docs/re/windows_save_serialize_re_findings.md`). `install_save_hook()` is
retargeted to it, observer-confirmed firing on the save worker thread (2 fires/save, correct AOB), and
the strip@entry/reinject@exit bracket is **wired + live** (`be7b212`): `strip_items()` →
`g_orig_ser(..)` → `reinject_items()`, synchronous, guarded by `g_in_serialize`,
`kItemStripReinjectWired=true`. `test_sidecar` 5/5 passes with the bracket live, no crashes.

**NOT yet proven:** that the bracket actually produces a clean on-disk vanilla save. Recipe: grant a
reserved-id item live (`give_item`) + register it (`sidecar additem`) → trigger a real game save →
reload with the `.mfg` `[items]` emptied → item must be GONE from the vanilla save. That assertion
needs an automated **`goods_count(id)`** read — now SOLVED:

**✅ `goods_count` FOUND + IMPLEMENTED 2026-07-03 (Windows-Ghidra, `docs/re/windows_goods_count_re_findings.md`).**
The blind 2-level `goods_diff` failed because the held qty is neither inline next to the id NOR ≤2 hops
out: ER uses GaItemHandle indirection AND the held list is a **two-segment split list three hops from
GameDataMan**. Ghidra (`D:\ghidra_proj2\ER`, new `tools/ghidra/find_goodscount.java` + `query.java`)
pinned the full layout: **`GameDataMan+8 → +0x2B0 EquipGameData → +0x158 EquipInventoryData` (carried)**;
segments (seg1_cap@+0x1C, seg1_base@+0x50, seg2_base@+0x40, last_index@+0x80), node stride `0x18`, node
`{handle@0 (0⇒empty), itemId@4 (0x40000000|goodsId), quantity@8}` (qty offset cross-checked via decrement
path `FUN_14024bfe0` + accessor `FUN_1407127a0`). Delivered **option (3), the direct read-only walk** (no
game call, no thread/save-timing risk) as `goblin::inventory::goods_count(id)`
(`goblin_inventory.{hpp,cpp}`, RPM-guarded, reuses `equip_game_data()`) + RPC `goods_count <id>` (reports
`err not in-world` vs a real `n=0`). Builds clean (clang-cl). Callable fallbacks recorded
(`FUN_14024c460`/`…c560` by-id finders).

**✅ goods_count offsets LIVE-VERIFIED 2026-07-03 (Linux/Proton).** Cross-built + deployed, then
`tools/rpc_tests/test_goods_count.py` (GameSession cold-boot → load save → grant/read) went 6/6:
fresh id `0x40003bed` reads `0→1→2→3` on repeated `give_item +1`; held id `0x40003bec` `7→8→9→10`.
Read tracks live held qty per-id, in-world. **Caveats found (give_item, NOT the read — full note in
`windows_goods_count_re_findings.md`):** AddItemFunc is ADD-ONLY (negative qty = no-op — the old
"−7 → 0" verify recipe was wrong; removal needs the remove path); `qty≥~5` clamps to the ~1000 stack
cap (grant N via N× `+1`); grants are live-inventory only, not persisted until a real save (fresh id
re-reads `0` after reboot → regression is idempotent).

**✅ Variant A clean-save CLOSED 2026-07-03 (Linux/Proton, E2E 4/4).** `tools/rpc_tests/test_custom_item.py`
(two cold boots: grant+additem+warp-save → empty `.mfg [items]` → reload) proves a registered custom
item does NOT survive in the vanilla `.sl2` once the `.mfg` stops re-granting it: boot-2 `goods_count==0`.
Three fixes made it work (the original bracket was a silent no-op):
- **Real strip (not `give_item(-qty)`).** AddItemFunc is add-only, so `strip_items()` now zeroes the
  matching EquipInventoryData node directly (`inventory::strip_goods()` — snapshot 0x18 bytes, write
  `handle@0=0`+`qty@8=0`; the exact decrement the game's `FUN_14024bfe0` does) and `restore_goods()`
  writes the bytes back the instant the serialize returns. The serializer honors the zeroed slot.
- **`WriteProcessMemory`-to-self silently FAILS on the inventory pages** (qty stayed 6 after a WPM
  strip) — a **direct in-process store under SEH** (`write_dw`/`write_bytes`) sticks. Use those, not WPM.
- **Idempotent reinject.** World-enter `reinject_items()` now grants only the missing delta
  (target − held) via the exact `give_item(+1)` primitive — a warp/area re-enter (item still live)
  grants 0 instead of inflating +1/save; a cold load (item stripped from the save, held=0) grants full qty.
Dev RPC `strip_test <id>` validates the strip round-trip WITHOUT a save (before→strip→0→restore→before).
Variant B (reserved-id item tolerated in the `.err`, no serialize hook) remains the zero-RE fallback.

**NEXT:** Gap C GRANT for arbitrary custom items can now build on this proven sidecar item (the Gap H
"don't dirty the `.sl2` until strip proven" contract is satisfied). Caveat still open: `give_item(+N)`
single-call is unreliable for N>1 (caps ~1000) — grant N via N× `+1` (reinject already does).

**Infra note (corrects stale memory):** a background Claude job CAN boot ER for a self-contained RPC
run — the missing piece was **Steam must already be running** (me3's `require_steam` aborts otherwise:
`ERROR require_steam: Steam is required to run this game`). Start it headless once with
`steam -silent` (auto-login persists, daemonizes, survives across tool calls), then `GameSession`
launches me3 as its in-shell child and kills the game at exit. See `mfg-rpc-driver-hardening.md`.

## Open / next items

- **Long-horizon vision bets — tracked in `docs/runtime_modding_framework_vision.md` "Future directions"
  (2026-07-03):** (1) World Virtualization (runtime switching between overhauls without reinstall —
  bundle-over-shared-base is the tractable slice; full overhaul-swap needs regulation/VFS virtualization),
  (2) In-Game World Editor (ImGui over the runtime primitives — the live-edit loop already EXISTS:
  `param_setf`/`param_clone`/`loot_at`/repoint/`lotItemId01`/`refresh_markers`; the editor is the panel
  wiring), (3) 3D model variants + reuse across worlds (asset/MSB frontier — needs an MSB-write path that
  doesn't exist; hardest/furthest). Not scoped; captured so they aren't lost.

- **Live marker regeneration (real-time map editing) — v1 DONE 2026-07-03; v2 open.** Markers build once
  at boot; to reflect a LIVE param edit on the DRAWN map without a game reload, **`refresh_markers` RPC**
  (→ `overlay_api::rebuild_markers` → `worldmap::rebuild_markers`, the production toggle-rebuild path) now
  forces a fresh bucket build. Verified: after a `pickUpItemLotParamId` repoint, `refresh_markers` ran a
  full `build.buckets` (2381 ms) on the detached disk WORKER thread (no frame freeze), re-reading live
  params; game alive. Since the rebuild uses the same live resolve as `loot_at`, existing-lot edits
  (repoint, `lotItemId01`, any param override) now show on the map. **Still open (v2):** (a) it's a FULL
  re-parse (~2.4s, re-walks every MSB) — INCREMENTAL regen (only affected buckets/tiles) for perf; (b) a
  NEWLY CLONED lot still won't resolve — the `LotReader` lot INDEX (`goblin_loot_resolve.cpp`) is
  snapshotted via `once_flag` at init and `rebuild_markers` doesn't reset it (existing lots reflect live;
  new lots need a LotReader-index reset). Gate any auto-trigger vs the collected-graying contract + the
  `read_wgm` cache-miss spike.
- **F1 panel to edit param overrides live** — optional polish on the param-override framework (all 3
  loader slices are done/merged); more registry fields = one AOB each. Not started.
- **Gap C GRANT — grant+sidecar PROVEN 2026-07-03; NAME + author surface remain.** A CLONED custom
  goods row grants into inventory and is kept out of the vanilla `.sl2` (`test_gapc_grant.py` 4/4 +
  boot-2 clean 1/1). Two findings baked into `custom_item_end_to_end_plan.md`: (1) **grantable goods-id
  ceiling `0x7FFFFE`** — `give_item` no-ops at ≥`0x7FFFFF`, so the old reserved band `90000001` was
  never grantable; use ≤`0x7FFFFE` (the test uses `8000000`). (2) **`fmg_set` slot: base `10`
  WORKS, DLC-tier `419` FREEZES** the present thread (RPC marshals there). Inject names at slot 10;
  the 419 hang + which slot the item-name UI reads are handed to a Windows/Ghidra sweep
  (`docs/re/windows_fmg_slot_re_prompt.md`). **RESOLVED (static, 2026-07-03,
  `docs/re/windows_fmg_slot_re_findings.md`):** goods-name UI (`FUN_140d10680`) reads
  `menu(111)→base(10)→dlc01(319)→dlc02(419)`, so a NEW id renders at base **slot 10** (111/DLC empty
  for it); the 419 freeze is our `patch_fmg_in_memory` doing a `fileSize − str_data_start` size_t
  **underflow** on a DLC-stub header (NOT the group loop — hence the reverted span guard didn't help)
  → multi-GB resize/memcpy on the present thread. Fix = O(1) offset/size sanity guard + reject slots
  ≥300 and the 11x menu tier; keep injecting at base 10. **DLL guards CODED + verified 2026-07-03**
  (`goblin_messages.cpp`: `patch_fmg_in_memory` offset/size + span-vs-stringCount guards;
  `inject_fmg_entries` slot policy): `fmg_set 419` now returns a fast error (game alive), `fmg_set 10`
  works, boot PlaceName(19)/TutorialBody(208) injects unaffected.
  **✅ Author surface DONE 2026-07-03** — `custom_items.toml` (TOML chosen over JSON for hand-authoring;
  toml++ header-only). `goblin_custom_items.{hpp,cpp}` applies each `[[goods]]/[[weapon]]/…` at boot
  (clone+fields+name) + `sidecar::register_author_item` (declarative registry: granted on world-enter,
  stripped pre-save, NEVER in the `.mfg` — re-applied every boot). E2E `test_author_items.py` 1/1: toml
  → boot → world-enter grant → `goods_count==qty`. Example `custom_items.example.toml`.
  **Remaining polish only:** finalize the reserved band from a param-scan survey; `decode_textid`
  read-back chain parity (menu-first `{111,10,319,419}`); more categories as needed. **Gap C is
  functionally complete.**
- **MapGenie coverage — Hidden Passage category, not started.** Hit-detected illusory walls, no action
  button → no static signal to parse (hardest remaining Group-2 category). RE notes:
  `docs/re/windows_group2_landscape_re_findings.md`.
- **MapGenie coverage — Wandering Mausoleum, not attempted.** Dynamic moving entity, no static MSB
  signal; low priority.
- **RPC auto-idle (`feat/rpc-auto-idle`) — needs in-game verify.** Built + deployed
  (`src/input/input_wndproc.cpp`, `goblin_debug_rpc.cpp`, ini `[Debug] rpc_auto_idle` default true):
  scripted RPC input (`key`/`mouse_*`) should self-suspend for ~1.5s when the human touches real
  keyboard/mouse, and NOT self-idle from its own injected input. Verify with the map open: wiggle the
  real mouse → `status` shows `rpc_input_idle=1` within ~1.5s and an RPC `key` is refused; stop
  touching input → resumes after ~1.5s; a scripted `type`/`key` run must NOT trigger it. Dev-only
  tooling, no changelog line on pass — just merge. Detail: `docs/memory/tooling/mfg-rpc-driver-hardening.md`.
- **Silent deadlock freeze — UNSOLVED.** One occurrence (2026-07-02): log goes silent (no crash, no
  exception), window solid, RPC thread alive. Distinct from the known `eldenring.exe +0x1EB9999` exit
  crash (that one's handled: TerminateProcess after triage). Shipped the catcher —
  **freeze watchdog** (`goblin_freeze_watchdog.cpp`, ini `[Debug] freeze_watchdog_secs`, default 20s):
  present-thread heartbeat; on stall writes `logs/MapForGoblins_freeze_<pid>.txt` + a full-thread
  minidump. **Next freeze → symbolize the dump with the deployed PDB and root-cause.**
- **Background-focus RPC driving — partially closed.** Root cause found: our own `g_has_focus` gate
  kills keyboard poll + mouse clicks off-focus (not the pause system). The first `key` after
  auto-refocus being silently lost is fixed (closed-loop retry via `hk_wndproc` arrival counter). Still
  open: `mouse_click`/`type` have no delivery-verify (same loss window), and "drive UI while the user
  works elsewhere" needs a dev-mode treat-as-focused override (accepted tradeoff: RPC keystrokes leak
  into the backgrounded game, symmetric with how PauseTheGame's global hotkeys already behave). Not
  started; until then keep the game window focused during scripted UI runs.
- **F2 fog-locate pan clamp — reverted fix, real bug still open.** Locating a target in undiscovered/
  fogged territory (e.g. Morgott while Leyndell is fogged) clamps the pan at the edge of revealed area
  instead of centering the target — deterministic repro documented. A fix attempt (direct pan/snap-rect
  writes, zoom-easer write) was REVERTED by user call; **read
  `docs/re/linux_f2_fog_locate_clamp_re_findings.md` before retrying** — the real blocker is the engine
  clamping the cursor reticle inside a `c32f0` step whose bounds source isn't in any struct we've found
  (needs Ghidra on the `c32f0` subtree). Hard constraint for any retry: non-fog locates must behave
  exactly as today, no per-frame write fights, no forced zoom.
- **Baked-data → runtime/disk migration — IN PROGRESS.** Authoritative plan:
  `docs/plans/baked_data_full_removal_plan.md` (6 phases; `build_pipeline.py` deletion is Phase 5, the
  END state, not the first step — it still generates tables with no runtime source). Landed: Phase 1
  (enemy-drop labels), name-alias English search (now reads live `msg/engus` off disk), several
  category-exception bakes recovered live via `EquipParamGoods.sortId`. **Next pick (easiest→hardest
  per the plan's inventory):** dedup `goblin_tile_tabs`/`goblin_major_regions` (identical across
  profiles now that there's only one profile — pure housekeeping); assess
  `goblin_region_anchors`/`goblin_name_regions` vs `WorldMapPointParam`+`WorldMapPlaceName`; the icon
  atlas (biggest remaining item, see next bullet). Minor unblocking follow-up noted, not gating: a
  handful of Reforged item families / DLC key items still fall into the "Loot - Crafting Materials"
  catch-all on colliding sortIds — needs dedicated rules or accept the catch-all.
- **Baked-atlas removal — DEFERRED, gate not passed.** `[ICONTIER]` census (kept in-tree for
  re-auditing) shows ~15 categories still resolve only through the baked atlas on ERR and/or vanilla
  (Hostile NPC, Spirit Springs, Stakes, Cookbooks, Crystal Tears, Golden Runes Low, …); until native/
  disk resolution covers those, the atlas stays. Re-run recipe and follow-up ideas in the file this
  replaced (`git log -p -- docs/HANDOFF.md` if needed) or re-derive via the `[ICONTIER]` census tool in
  `map_renderer.cpp`.
- **Lag-spike hunt — `read_wgm` cache-miss path still spikes.** The steady-state RB-tree walk was fixed
  (bulk RPMs, `read_rb` helper) and AVG dropped to ~0.05ms, but fresh-tile loads still spike 2-3ms
  (~33x) because every new tile re-reads each geom instance's full chain (~3 RPMs/instance) before the
  AEG family filter drops the noise. Next ideas: budget cache-miss resolution per refresh (check the
  collected-graying contract first so a deferred tile doesn't flash wrong), or land an AOB-pinned
  O(1) collected getter to skip the RPM snapshot entirely (`goblin_collected.cpp:543` already has a
  DR0 armed for this). NB `present.overlay_total`/`present.newframe` spikes were investigated and
  RESOLVED AS WONTFIX — game-side frame cost and a one-time ImGui font-atlas upload, not our code.
- **Map-exit input softlock — external cause, low priority.** Root cause is Deskflow (cursor-sharing
  KVM), not this mod or ER; fix is Deskflow-side. `docs/re/windows_input_softlock_re_prompt.md`.
- **Open policy question: is non-ERR/vanilla a hard support target?** Decides whether ERR-leaning bakes
  (atlas, etc.) can eventually be dropped entirely or must stay as a permanent vanilla-compat net.
- **Double-DLL-load hardening — not implemented.** Strategic fix (single-DLL migration) landed and
  prevents NEW installs from double-loading, but an existing install with a stale `_vanilla.dll` still
  can. TODO: a named-mutex check (`CreateMutexW`) at init so a second instance bails before installing
  any hooks and shows an on-screen "double load detected" banner instead of silently double-drawing.
- **Clang-only toolchain — Phase 1 mostly done, matrix open.** `build.bat` (ninja+clang-cl) and
  `build.bat snapshot` are both validated on Windows (packaging + PDB archival proven). Still open:
  `build.bat release` (version-bump path) unexercised; Phase 2's real in-game validation matrix.
  `docs/plans/clang_only_toolchain_plan.md`.
- **Big-files refactor — items 1+2 done, 3-7 open.** `docs/plans/big_files_refactor_plan.md`: done =
  panel split into `src/overlay_panel/`, shared marker gates. Remaining: classify dedup, diag
  quarantine, `icon_uv`, god-function breakup, grace-sprite design.
- **Real map clipping (RE the game's own clip) — not started.** Would replace the exclusion-zone
  stopgap (dial disc + user rects) with pixel-perfect clipping identical to the game's own map/minimap
  clip. Big RE; low priority, current stopgap works.
- **Zoom+pan simultaneous 1-frame icon "dash"** — stale projections streak icons for a frame when zoom
  and pan change together. Suspect: the ViewDelay ring interpolating pan/zoom inconsistently. Not
  investigated.
- **Fan (spiderfy) near a screen edge can overflow off-screen** — the canvas clip trims it but doesn't
  re-anchor the fan. Minor, not investigated.

### Decided against (don't re-propose without reading why)

- **Merchant map pins (search Slice 3) — SHELVED 2026-07-03 after an RE spike.** The shop↔NPC join is
  talk-ESD-only (confirmed no EMEVD `OpenRegularShop` signal exists); pulling a shop-id range out of ESD
  needs a full EzState bytecode evaluator — disproportionate for one pin category. Merchant item
  *search* (Slice 1) is the shipped feature; naming the seller (Slice 2) was separately deferred for
  the same ESD reason. `docs/plans/merchant_item_search_plan.md` Slice 3.
- **F2 zoom-easer write fix — REVERTED.** Mechanically worked but forced an uninvited zoom on every
  fog-locate plus a visible flicker fight on clamped targets; user rejected the UX tradeoff. See the F2
  entry above for the real fix direction.

## Standing gotchas & non-obvious facts

- **RPC/driver scripting gotchas** (full detail `docs/memory/tooling/mfg-rpc-driver-hardening.md`): a
  background job can only keep ER alive via a single FOREGROUND blocking bash command (me3 as an
  in-shell child, killed before return); `ping` ≠ game alive, gate on real liveness; AZERTY layout means
  SendInput's VK→scancode uses US but the return scan→VK translation uses the HOST layout, so scripted
  `type` must send QWERTY-position characters, not the intended letters; `mouse_move` needs the
  SetCursorPos trampoline + a real ±1px jiggle event (absolute SendInput alone lands off-target and the
  game re-warps the raw-input reticle onto the old position after one frame) — send it twice, a rare
  warp race eats the first; `pkill -f "Game/eldenring.exe"` also matches the driver shell's own args and
  kills it early.
- **Wineserver RPM contention:** many small `ReadProcessMemory` calls in a hot per-frame path can
  contend with the render thread even at sub-ms each, because wineserver serializes ALL RPM calls
  process-wide under Wine — batch reads into as few RPM calls as possible (lesson from the `read_wgm`
  spike fix).
- **Double-DLL-load is not a code bug.** If both an ERR and vanilla DLL variant ever end up in the mods
  folder, both load into the same process → doubled ImGui draw, doubled PlaceName patch, etc. Single-DLL
  migration prevents this for new installs; see the hardening TODO above for stale existing installs.
- **AOB doctrine:** pin code sigs, never raw RVAs — the WorldChrMan resolver was flipped from
  RVA-first to AOB-first after an audit found it violating this (a future ER patch that moves the slot
  would otherwise silently go stale). `goblin_world_position.cpp`.
- **Grace icon scale is deliberately SEPARATE from the generic marker scale** — calibrated for vanilla
  parity when the cursor locks onto a grace; do not fold it into the shared scale knob.
- **Golden-rune glow sizing:** size any glow/backing effect off the icon's NATURAL draw size (`base_hh`,
  a ratio), not the post-bump scaled size — sizing off the scaled size produced a big dim wash instead
  of a compact bright orb.
- **The 7 mod-added POI categories** (Spirit Springs / Summoning Pools / Stakes / Material Nodes / Bell
  Bearings / Interactables / Spiritspring Hawks) have no ERR-custom glyph; their massedit iconIds
  (374+) point at glyphs absent from every current menu file (numeric glyphs cap at 261) — recover via
  a real `SB_MapCursor` glyph where one visually fits, else circle.
- `MENU_MAP_ERR_*` (boss/grace) names are ERR-only; they won't resolve off-ERR → falls back to circle
  if the baked fallback is ever removed.
- Offline KRAK decompress works on Linux via `internals/launcher/liboo2corelinux64.so.9`.
- Extracted glyph sheets (gitignored scratch): `tools/extracted/*.png` — regenerate via
  `bash tools/build_menu_tex_extract.sh && ./tools/menu_tex_extract`.
- i18n: `overlay_language = auto` reads the WINE prefix locale under Proton (usually `en_US` even on a
  French desktop) — French users should set it explicitly. Avoid `œ` in translations (outside the
  merged font ranges, use "oe"). Keep label translations ≲ English+20% (panel caps at 840px).
