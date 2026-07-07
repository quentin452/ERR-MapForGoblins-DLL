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

## ★ Rescue-hook RE — the last mile (Ghidra `D:\ghidra_proj2\ER`)

Goal: at load, if the spawn MapId is invalid (area byte 0), force a safe spawn (The First Step) instead of
wedging — the user's ask "si le warp est invalide, teleport to the First Step".

1. **Identify `r14`** in the function containing `er+0x627ffc`: walk its prologue to see how `r14` is
   derived (a param, or a field of a session/world-info struct). `r14` (or what feeds it) IS the authority
   holding `0x000B0000` at load.
2. **Find the save-load writer of the authority** — where the `.err` blob's saved location lands in the
   `r14` struct. FWA-write that field DURING a poisoned load (arm BEFORE `Continue`; a preserved poisoned
   save is `ER0000.err.POISONED-void-…` / `…-lakefall-…` next to the save dir). That writer reads the save.
3. **Hook + validate:** detour the authority writer (or `er+0x627ffc`); if the incoming MapId `area==0`,
   substitute First Step's MapId **and** position (BonfireWarpParam / the known m60_42_35 spawn), so both the
   mirror and the streamer agree on a loadable location. Validate by `mem_write`ing the fix during a live
   wedge first (authority field, not the mirror) — if the load completes, the hook point is proven.

## Method notes (reusable)

- `mem_fwa` is DR0-only + one-shot; `mem_fwa off` disarms so you can re-arm a new address same session.
- The MapId singleton is **null at the main menu** — it is allocated during load, so `singleton+0x2c`
  cannot be pre-armed at the menu. Catch the getter by write-watching the static SLOT (exists at menu);
  catch the writer by write-watching `singleton+0x2c` in-world + crossing a tile (the value is otherwise
  stable while standing still).
- Open curiosity (not blocking): a raw `mem_dump` of `singleton+0x2c` in-world once read `0x12000000`
  while `coords` decoded area 60 — a transient/torn read mid-update; the probe reads at a settled point.
