# Infinite-load wedge — the invalid-spawn MapId chain (writer + getter caught live)

**Status: SOLVED to the WRITER; rescue-hook RE is the last mile.** exe fingerprint `er_version = 2.6.2.0`
(the `er+RVA` sites below are relative to `eldenring.exe` base — re-verify the version before trusting them).
Captured live 2026-07-07 via `mem_fwa` (DR0 find-what-accesses) on the dev box.

## The bug this explains

Three saves got POISONED on 2026-07-07 (coordinate-teleport / fall incidents): the player fell OUTSIDE any
mapped volume, an autosave landed while the engine's current-map tracker was INVALID, and every subsequent
`Continue` hangs on the loading screen forever (LocalPlayer null, present thread alive at 60fps). Isolation
test: the ENGINE ALONE wedges (MapForGoblins.dll renamed away) → the save content is genuinely unloadable,
not a mod artifact. The sidecar save/serialize hook was NEVER installed this box (`sidecar_save=false`), so
our write path is cleared too. See `docs/HANDOFF.md` "LOAD-WEDGE LIVE RE".

During a wedge, the MapId singleton's current-map field reads **`0x000B0000`** (area byte 0 = nonexistent
map). That is the user's "warp inconnu", byte-exact: the loader chases a map that does not exist.

## Anchors (live, from the boot log)

- **MapId singleton slot** (`PLAYER_MAPID_SLOT` AOB): `er` data slot, this boot `0x7ff7d5ed91d8`.
  `*slot` = the singleton (heap, per-boot; this boot `0x1e558531580`; vtable `0x7ff7d4be8d98`).
- The engine reads the current MapId at **`singleton + 0x2c`** (u32, `(area<<24)|(gx<<16)|(gz<<8)|idx`).
  `probe_map_pos_body` in `goblin_world_position.cpp` uses exactly this.

## ✅ Site 1 — MapId singleton GETTER: `er+0x6190c0` (FD4Singleton lazy-get)

Caught by write-watching the SLOT during a healthy load (the getter re-stores the instance):

```
FWA hit: WRITE of <slot> by rip=er+0x6190c0
bytes[rip-24..rip+16) = ... 48 8B 05 <disp32>   mov rax,[slot]      ; load current
                            48 85 C0             test rax,rax
                            48 0F 44 C6          cmove rax,rsi       ; if null → newly-alloc rsi
                            48 89 05 <disp32>    mov [slot],rax      ; <-- the watched write
caller ret-addrs: er+0x3294dc0 er+0xaf8fc9 er+0x29c8e58 er+0x29c8e58 er+0xaaca9b er+0xaf4c27 er+0xa9e0d2
```

Textbook `CS::FD4Singleton::GetInstance` idiom → `er+0x6190c0`'s function is the MapId-singleton getter.
Callers `er+0xaf8fc9 / 0xaaca9b / 0xaf4c27 / 0xa9e0d2` = the load/session-setup cluster.

## ✅ Site 2 — MapId WRITER (the prize): `er+0x627ffc`, watch trips at `rip=er+0x627fff`

Caught by write-watching **`singleton+0x2c`** while the player crossed a map-tile boundary (grid 42→41 in
west Limgrave). This is the funnel that commits the current MapId into the singleton mirror:

```
FWA hit: WRITE of <singleton+0x2c> by rip=er+0x627fff
bytes[rip-24..rip+16) = ... 41 8B 06             mov eax,[r14]       ; eax = MapId from the SOURCE (r14)
                            48 8D 4C 24 50       lea rcx,[rsp+0x50]
                            49 8B D6             mov rdx,r14
                            89 47 2C             mov [rdi+0x2c],eax   ; <-- store into singleton+0x2c
                            E8 6C 93 67 00       call er+0xc8f370
caller ret-addrs: er+0x1b77bd er+0x622d03 er+0xb014f4 er+0x3e9a16 er+0xaff9d1
```

**Key: `rdi` = the singleton, and `eax` is loaded from `[r14]` — so `r14` is the UPSTREAM AUTHORITY** the
mirror copies from. `singleton+0x2c` is a derived mirror (proven earlier: `mem_write`ing it valid during a
wedge does NOT unwedge — the streamer reads the authority, not the mirror). Callers `er+0xaff9d1 / 0xb014f4`
overlap the getter's load cluster → same map/session update path, so the LOAD restore of the spawn MapId
almost certainly flows through `er+0x627ffc` (or its `r14` source) too.

## ✅ Ghidra decomp of the writer + its callers (2026-07-07, `D:\ghidra_proj2\ER`, exe 2.6.2.0)

`r14` = `param_2` of the writer = a pointer to the incoming MapId. The functions, top-down:

- **`FUN_140627fc0` = `SetCurrentMap(worldInfo* p1, int* mapId, u32 subId)`** — the map-change FUNNEL.
  `if (p1[0x2c] != *mapId) { p1[0x30]=p1[0x2c]; p1[0x2c]=*mapId; FUN_140ca1370(); if(changed){
  FUN_14062a120(p1); if(DAT_..d69d98)FUN_1406eaaf0(); if(DAT_..d66258)FUN_1405140b0();
  if(DAT_..d68838)FUN_1405fb2b0(); } } p1[0x28]=subId;` — i.e. it writes the mirror AND fires the
  change-notifications that propagate to streaming. This is why a bare `mem_write` of `+0x2c` never
  unwedged: no notification fired. `worldInfo` fields: `+0x2c` mapId, `+0x28` subId, `+0x30` prevMap,
  `+0x180` position, sub-ptrs `+0x18/+0xe8/+0xf0`.
- **`FUN_140622c70(worldInfo, mapDescriptor)`** — applies a map DESCRIPTOR: reads `desc+0x84` (mapId,
  when `desc[0x10]==0`) / `desc+0x83` (byte→subId), stores `desc` at `worldInfo+400`, calls the setter.
  The tile-crossing path (the FWA caller chain: setter ← `FUN_140622c70` ← `FUN_140b012d0`).
- **`FUN_1406260e0 = SetPlayerMapAndPos(worldInfo, int* mapId, u32, u8, short* posData)`** — ★ the
  spawn/warp APPLIER: sets the map (`local_94=*mapId; …; FUN_140627fc0(worldInfo,…)`) AND the position
  (`worldInfo+0x180 = *posData`). Callers `FUN_140625670 / FUN_140a9e810 / FUN_140a9e890`. **This is the
  best rescue hook — it carries BOTH the map and the position**, which is exactly what a poisoned save
  corrupts (area-0 map + fall position).
- **`FUN_140b012d0`** — per-frame/transition map updater: `sess=FUN_140507ff0(); desc=FUN_1403ef9f0(sess);
  cur=FUN_14061f780(worldInfo+0xf0); if(desc!=cur){ FUN_140622c70(worldInfo+0xf0, desc); copy pos →
  (worldInfo+0xe8)+0x30 }`. Reads the session's TARGET map descriptor (`desc+0x84`=mapId) and commits it.

The invalid `0x000B0000` therefore lives in a **map-descriptor's `+0x84`** (or the `int* mapId` an applier
is handed), loaded from the `.err`. Fixing the mirror is useless; fix the value BEFORE/AT an applier so the
notifications carry a valid map.

## ★ Rescue-hook RE — the last mile (Ghidra `D:\ghidra_proj2\ER`)

Goal: at load, if the spawn MapId is invalid (area byte 0), force a safe spawn (The First Step) instead of
wedging — the user's ask "si le warp est invalide, teleport to the First Step".

1. ✅ **`r14` identified** — `param_2` = the incoming MapId pointer; the applier `FUN_1406260e0` carries
   both map (`param_2`) and position (`param_5`). (Done above.)
2. **Which applier does the LOAD use? (next boot, poisoned save — the decisive test.)** Arm FWA on
   `FUN_1406260e0` ENTRY (`er+0x6260e0`) BEFORE `Continue`, load a poisoned save
   (`ER0000.err.POISONED-void-…` / `…-lakefall-…` next to the save dir), and see if it fires with the
   invalid map:
   - **Fires** → hook `FUN_1406260e0`: if `*param_2` area byte == 0, overwrite `*param_2` (mapId) → First
     Step's MapId and `param_5` (posData) → First Step's pos. Complete rescue (map + pos) in one place.
   - **Doesn't fire** → arm the setter `FUN_140627fc0` (`er+0x627fc0`); if THAT fires, hook it for the map
     and find the separate position writer (`worldInfo+0x180`) to fix pos too.
   - **Neither fires** → the streamer reads the descriptor (`desc+0x84`) before any applier; FWA-write
     `desc+0x84` during the poisoned load to catch the save-parse that fills it, and hook there.
3. **Prove before shipping:** `mem_write` the fix into the live authority during a wedge (the descriptor
   `+0x84` / the applier's incoming mapId, NOT the mirror `+0x2c`) — if the load completes, the hook point
   is confirmed. First Step MapId = `m60_42_35` = `0x3C2A2300`; grace/pos from BonfireWarpParam (row for
   The First Step, `bonfireEntityId 1042362951`).

## ✅/❌ Rescue attempt 1 — hook the map-SETTER: DEAD END for the LOAD (2026-07-07 live test)

Built `goblin_load_rescue.cpp` (MAP_SETTER AOB → hook FUN_140627fc0; log every call; when armed,
area==0 → rewrite `*mapId` to First Step 0x3C2A2300). RPC `load_rescue [status|on|off|verbose|set]`.

- **Healthy load: the setter FIRES** — `[LOADRESCUE] setmap in=0x3c2a2400 area=60` (the save's map,
  committed via this funnel). So the setter IS the live map-commit for tile crossings + valid loads.
- **Poisoned load (void save, armed): the setter NEVER fires** — `load_rescue status: last 0`, yet the
  singleton `+0x2c = 0x000B0000` (read live during the wedge). So on a poisoned load the invalid map is
  written to `+0x2c` by the **load-restore path, NOT the setter**, and streaming HANGS on the area-0 map
  **before** FUN_140627fc0 is ever called. The present thread keeps rendering the loading screen
  (`frame=` advances) → it's an infinite LOAD, not a freeze.
- ⇒ **the wedge is UPSTREAM of the map-commit setter.** Hooking the setter cannot rescue the load. The
  `load_rescue` module stays as a live map-change DIAGNOSTIC (and would fix a runtime-induced area-0
  live map-change), but it does NOT fix a poisoned SAVE's load. Disarmed by default.

## ★ Rescue attempt 2 targets (next session) — intervene UPSTREAM of the setter

1. **Offline save-repair (most promising, no runtime RE).** `ER0000.err` is **BND4 plaintext** (magic
   `BND4`; not encrypted at the container level). The invalid `0x000B0000` appears 33× in the void save,
   CLUSTERED at ~`0x299cf5/d35/d55/d75/d95/db5` (stride 0x20 = a TABLE of map entries all set to area 0 —
   the map history/recent-maps got poisoned, not one field). A raw poisoned-vs-healthy byte diff is 10.7%
   (no direct-parent healthy snapshot exists → too noisy to isolate the spawn field). NEXT: parse the
   BND4 with SoulsFormats (`tools/lib/Andre.SoulsFormats.dll`) → the PlayerGameData/GameMan slot →
   locate the spawn `mapId` field by the ER save layout, rewrite area-0 → First Step, repack. Ship as a
   `tools/` save-repair script (user runs it on a wedged save; robust, offline).
2. **Runtime: the load-restore map-request.** The function that reads the save's spawn map and REQUESTS
   it from streaming (upstream of FUN_140627fc0). It writes `+0x2c` directly on load. Hard to FWA (the
   singleton is null at menu; the write is once, during load, before we can arm). Approach: hook the
   getter er+0x6190c0 (singleton allocator, runs during load) and from the detour auto-arm a DR0 write-
   watch on the fresh `singleton+0x2c` to catch the load-restore writer next boot; or Ghidra-trace the
   save-load/deserialize (the load counterpart of SERIALIZE_FN) to the spawn-map apply.

## Method notes (reusable)

- `mem_fwa` is DR0-only + one-shot; `mem_fwa off` disarms so you can re-arm a new address same session.
- The MapId singleton is **null at the main menu** — it is allocated during load, so `singleton+0x2c`
  cannot be pre-armed at the menu. Catch the getter by write-watching the static SLOT (exists at menu);
  catch the writer by write-watching `singleton+0x2c` in-world + crossing a tile (the value is otherwise
  stable while standing still).
- Open curiosity (not blocking): a raw `mem_dump` of `singleton+0x2c` in-world once read `0x12000000`
  while `coords` decoded area 60 — a transient/torn read mid-update; the probe reads at a settled point.
