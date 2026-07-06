# RE prompt — co-op player list (freeze gate + player markers), ELDEN RING Reforged / Seamless Co-op

**Goal:** one accessor `goblin::coop` exposing **(a)** `others_present()` — is ≥1 OTHER player in this
session — and **(b)** a list of the other players `{ChrIns*/handle, world pos, name/SteamID}`. Dual-use:
- **Freeze gate:** the fullscreen-vmap world freeze (`SetDisableAllChrUpdate`, game_timestep_freeze_re_findings.md
  "SOLVED") is a LOCAL sim freeze → in co-op it desyncs (a frozen client stops simulating networked entities;
  a frozen player may not take damage). So **skip the freeze when `others_present()`** (solo keeps it).
- **Markers:** draw the co-op partners as live blips on the WorldMap + Minimap (their world pos → the existing
  overlay marker projection).

## Recon done (2026-07-06, Linux — deployed `ersc.dll`)
- Mod = **Elden Ring Seamless Co-op v1.9.9 by Yui** (`/home/iamacat/Games/ERRv2.2.9.6/dll/offline/ersc.dll`,
  7.79 MB). Source is CLOSED (the `LukeYui/EldenRingSeamlessCoopRelease` repo ships binaries only, despite the
  `ersc\buddy\buddy.cpp` / `ersc\seamless_session_manager\seamless_session_manager.cpp` debug paths baked in).
- **"buddy" = a connected co-op player** (string `"seamless buddy system"`; `ersc\buddy\buddy.cpp`).
- `ersc.dll` exports only ONE symbol (ordinal 1, RVA 0x2b00 = the ModEngine entry) → **no clean player API**.
- Native `CS::CSSessionManager` + `SprjSessionConnectType` are referenced, but seamless spawns buddies via its
  OWN buddy system, so CSSessionManager may not reflect the buddy count — unverified.

## Two approaches (do B first)
### B — count player ChrIns in WorldChrMan (ER-native, NO ersc RE) ★ recommended first
Seamless renders each buddy as a real in-game character → each buddy is (almost certainly) a `CS::PlayerIns`
(or a networked-player ChrIns) inside WorldChrMan. If so:
- `others_present()` = count of `.?AVPlayerIns@CS@@` ChrIns (or net-player class) **> 1** (minus the local
  player at `[[er+0x3d65f88]+0x1e508]`).
- Marker positions = each such ChrIns's world pos (same chain the player-dot uses:
  `[[[[player+0x58]+0x10]+0x190]+0x68]+0x70/74/78`, windows_yellowdot_player_pos_re_findings.md).
- Reuses the WorldChrMan block walk already RE'd (WCM+0x1CC58 count / +0x1CC60 block array, stride 0x18;
  block+0x18 EnemyIns pool is volatile, but PLAYERS may live in a cleaner set — check the player ChrSet, and
  whether buddies are in the same block+0x18 pool or a dedicated player list). **RTTI-filter to PlayerIns.**
- **Cheapest + dual-use in one.** Code it blind (count PlayerIns), but CONFIRM "buddy == PlayerIns in WCM" +
  read their positions on a LIVE 2-player session.

### A — reverse the seamless buddy list in ersc.dll (fallback, Windows Ghidra)
If buddies are NOT WorldChrMan ChrIns (e.g. seamless keeps them in its own struct only): Ghidra on
`ersc.dll` v1.9.9. Anchor via the strings `"seamless buddy system"`, `seamless_session_manager.cpp`,
`buddy.cpp` → find the buddy-list global (a vector/array of buddy structs: SteamID + player handle/ChrIns +
world pos). AOB-scan ersc.dll (base = `GetModuleHandleA("ersc.dll")`). Heavier + against closed code.

## Validation (blocks both) — needs a LIVE 2-player session
Solo always reads count=1 / no buddies, so `others_present()` can be coded blind but only CONFIRMED with a real
co-op session (2 machines / 2 players). Checklist: join co-op → `others_present()` flips true; each buddy's pos
tracks their movement; open the vmap → freeze is SKIPPED (partner keeps moving, no desync); markers draw at the
buddies' live positions.

## Related, already DONE
- **Auto-close the vmap on death / world-not-playable** (`goblin_overlay.cpp`): while the vmap redirect is
  open, if `get_player_world_pos()` reads false (LocalPlayer null — death→reload, area transition, quit-out)
  for 2 consecutive frames, the vmap force-closes (and its freeze releases). Matters in co-op (the vmap isn't
  frozen there, so you can die with it open); harmless solo (the freeze prevents dying with it up). Testable
  solo by triggering a loading screen (warp) with the vmap open.

## Wiring once resolved
- `goblin_overlay.cpp` vmap gate: `request_freeze(FREEZE_VMAP, vmap_redirect() && !goblin::coop::others_present())`.
- Markers: feed `goblin::coop::players()` into the WorldMap/Minimap marker pass (new "co-op player" marker type).
- Interim (no RE): a config toggle `vmap_freeze` (default ON) so co-op players disable the freeze by hand.

Cross-ref: `game_timestep_freeze_re_findings.md` (the freeze), `windows_yellowdot_player_pos_re_findings.md`
(player-pos chain for marker positions), `entity_radar_foundation.md` (WorldChrMan enumeration foundation).

## Ghidra recon (2026-07-06, Windows) — WorldChrMan ChrSet map (approach B foundation)
Confirmed **it is Seamless drawing co-op players on the map, not vanilla ER**: no player/co-op pin class in
RTTI (the `WorldMapPinData` family is only Warp=grace / SignPuddle / Point, all param-driven statics), and
`ersc.dll` isn't even loaded on the Windows box (only `me3_mod_host.dll` + `er_console_mod.dll`; only
`ersc_settings.ini` on disk). ⇒ **don't depend on ersc — read buddies as `PlayerIns` from WorldChrMan**
(mod-agnostic per the prime directive; buddies spawn as real chrs).

**WorldChrMan ChrSet layout** (`WorldChrManImp` ctor `FUN_140503e70`; offsets from WCM = `[er+0x3d65f88]`):
```
WCM+0x10EE0  ChrSet       (FUN_1404921b0)   ┐ 4 plain ChrSets — one is the PLAYER set
WCM+0x10F38  ChrSet                          │ (others = enemy/ghost/debug); which one
WCM+0x10F90  ChrSet                          │ = NOT yet IDed (needs a live dump).
WCM+0x10FE8  ChrSet                          ┘
WCM+0x11040  OpenFieldChrSet (FUN_1404eb5e0)   ← all open-field chrs (players+enemies); RTTI-filter this.
WCM+0x1E508  LocalPlayer* (PlayerIns)          (known)
```
**ChrSet struct** (`FUN_1404921b0`): `+0x00` vtable `CS::ChrSet::vftable`, `+0x08` id, **`+0x28/+0x30` container
A**, **`+0x40/+0x48` container B** (std containers of ChrIns — begin/end walk TBD; cleaner than the volatile
`block+0x18` pool). **`PlayerIns` RTTI vtable = `er+0x2a7cb40`** (the filter anchor; ctors 0x6507a0/0x64fe40).
Related: `PlayerNetworkSession` (vtable er+0x2b9eb30, per-player net obj, not the list), `NetChrSetSync`
(er+0x2a46bd0 — networked chr-set sync, the co-op path).

### ★ SOLVED (2026-07-06, Windows live RPM, solo-validated) — the player/session array
Used the LocalPlayer as a probe: scanned each ChrSet's pointers for `[WCM+0x1e508]` → it lives in **ChrSet#0
(`WCM+0x10EE0`), array field `+0x18`**. So:
```
player array   = *(WCM + 0x10EF8)          (WCM+0x10EE0 = ChrSet#0 struct, +0x18 = ChrIns* array)
capacity       =  *(int*)(WCM + 0x10EF0)   (= 6  →  local + up to 5 others = ER co-op session cap)
slot[i]        =  ((ChrIns**)array)[i], i in 0..5
```
Live solo read: **slot[0] = LocalPlayer**, its vtable **== `er+0x2a7cb40` (PlayerIns) ✓**, position via the
existing chain = `(-1.4, 3.7, -4.4)` ✓; slot[1] = `0x404` junk, slot[2..5] = null. (Note `WCM+0x10EF8` is the
old "LocalPlayerOffset 0x10EF8" long noted DEAD — it was never the local player, it's the **player array**.)

**Implementation (mod-agnostic, no ersc, positions REUSE the existing player-pos chain — zero new pos RE):**
- `others_present()` = over slots 0..5, count entries whose `*(entry) == er+0x2a7cb40` (PlayerIns vtable),
  **minus 1** (the local). The RTTI check rejects junk (`0x404`) cleanly — don't just null-check.
- `players()` positions = each valid entry → `[[[[*(entry+0x58)]+0x10]+0x190]+0x68]+0x70/74/78` (the
  yellow-dot chain, `windows_yellowdot_player_pos_re_findings.md`) → feed MFG's overlay marker projection.
- Resolve WCM live every call (`*(er+0x3d65f88)`, ASLR + reallocs on transitions).

**Only remaining co-op-dependent unknown:** that buddies actually populate slots 1-5 in a live seamless
session (very likely — it's the session player array, cap 6). Validate on a 2-player session: `others_present()`
flips true, each buddy slot's pos tracks their movement, markers draw. Scripts: `scratchpad/find_player_set.py`
(the probe) + `validate_player_set.py` (the array read/RTTI/pos).
