# Player-position WRITE (teleport) — the working coordinate teleport (havok body write)

**Status: SOLVED + LIVE-VERIFIED (2026-07-07, Windows/ERRv2.2.9.6). Coordinate teleport now works.**
The working teleport writes the player's HAVOK PHYSICS BODY position directly (er_console_mod's method,
RE'd from its DLL) — NOT the `LocalPlayer+0x6C0` mirror, and NOT the engine `SetPos` call (that alone
does not complete the warp from the RPC thread; see §"SetPos — the dead end"). Implemented as
`goblin::warp::teleport_coords` and wired into `warp_local`/`warp_xyz` + the vmap click-to-warp.
**Live-verified**: `warp_xyz` moved the player +20 m and it HELD (no snap-back).

## ★ THE WORKING TELEPORT — write the havok body Vec3 (er_console_mod's `tp`, RE'd + verified)

The authoritative player position is a Vec3 on the ChrIns physics module, reached by:
```
module = *(LocalPlayer + 0x190);   // the ChrIns physics/anim module (same +0x190 as the HP chain)
posObj = *(module     + 0x68);     // position holder
Vec3   @ posObj + 0x70 (X) / +0x74 (Y=height) / +0x78 (Z)   // +0x7C = w = 1.0
```
**Writing this Vec3 MOVES the body and HOLDS** (verified live: wrote havok X += 20 → the player moved +20 m
and stayed; a `coords` a beat later confirmed the new position, no snap-back). This is exactly what
er_console_mod's `tp` command does — decompiled from `er_console_mod.dll` (imagebase 0x180000000): the
handler at RVA `0x9700` reads `entity+0x190` then `+0x68` via a safe-deref helper (`FUN_1800084a0`) and
writes the three parsed floats to `+0x70/+0x74/+0x78` via a safe-write-float helper (`FUN_180008500`).

**Frame:** the body Vec3 is in a havok/physics-block-LOCAL frame, offset from the `LocalPlayer+0x6C0`
tile-local frame by a per-block origin (live: body=(-1.006, 3.547, -3.805) vs tile-local=(-1.01, 91.55,
-75.80) — same X, Y/Z offset by the block origin). The two differ ONLY by a translation, so a **delta maps
1:1** (havok X += 20 → tile-local X += 20). To reach a tile-local target T from the current tile-local
`L` and body `H`: `body_target = T - (L - H)`. `teleport_coords` reads both live and applies this.

**Thread:** the write is safe from the RPC/present thread (live-verified via `mem_write`) — no main-update-
thread marshalling needed (unlike SetPos). It's a plain struct write, SEH-guarded.

## SetPos — the dead end (kept: it explains the mirror, but it does NOT teleport from the RPC thread)

The `+0x6C0` mirror analysis below is correct and useful, but the engine `SetPos` call built on it did
NOT actually move the player when driven from the mod (live: `teleport_coords` via SetPos logged "called",
no fault, but a `coords` after still showed the OLD position — same snap-back). SetPos arms `+0x160|0x80`
and stages `+0x6C0`, but the per-frame consumer's body-drive (`FUN_140dc6600`→`FUN_140dc8150`) never
completed the move — the mode/registration in `FUN_140dc6e90` (branch on `ChrIns+0x30`) or the scene-apply
in `dc8150` needs context the RPC-thread call doesn't provide. Rather than chase that state machine, we use
the direct body write above (simpler, proven, and what a shipping teleport mod already does). The SetPos
ABI is documented below for the record.

## Symptom

`warp_local <x y z>` and `warp_xyz <worldX worldZ>` write `LocalPlayer+0x6C0/+0x6C4/+0x6C8`
(`goblin::write_player_local_pos`, `goblin_world_position.cpp`). The write LANDS — the read-back
inside the same verb returns the target. But a `coords` a moment later shows the OLD position: the
player never moved. The mod's doc called these "the first player-pos WRITE"; that was wrong.

## Root cause — `LP+0x6C0` is an OUTPUT MIRROR / warp-staging buffer, not the live position

The live physics/havok body position is authoritative; `LP+0x6C0` is written FROM it every frame, so a
raw store there is clobbered on the next physics tick. It is also the engine's own warp-STAGING field.

Static scan of `eldenring.exe` for stores to `[reg+0x6C0]` (capstone over `.text`,
`tools/…/find_pos_writer2.py`) found the SetPos path:

```
er+0xdc6380  SetPos(ChrIns* rcx, Struct* rdx, r8, bool r9b):
  48 83 ec 38            sub  rsp,0x38
  80 89 60 01 00 00 80   or   byte [rcx+0x160], 0x80      ; set "position pending/dirty" flag
  0f 28 42 30            movaps xmm0,[rdx+0x30]           ; source pos xmmword (x,y,z,w) at rdx+0x30
  0f 11 81 c0 06 00 00   movups [rcx+0x6c0], xmm0         ; stage into +0x6C0
  f3 0f 10 05 …          …                                ; then call er+0xdc6e90 (propagate to physics)
```

- Only caller: `er+0xda797b` (`rcx=rdi` = the ChrIns; `rdx=lea [rbp-0x50]` local with pos at +0x30;
  `r8=[rbp-0x80]`, `r9b=1`).
- The CONSUMER `er+0xdc6600` gates on `cmp byte [rcx+0x160], 0x80` — confirms +0x160 bit 0x80 is the
  "teleport pending" trigger the setter arms.
- The only other `+0x6C0` store, `er+0xdc761c`, is an ADD-delta integrator (`addss xmm0,[rcx+0x6c0]`
  then store) — i.e. the per-frame physics writer that overwrites our raw store. That's the mirror.

So a working teleport = write +0x6C0 **AND** arm `+0x160 |= 0x80` **AND** run the propagate call — i.e.
call `er+0xdc6380`, not a raw store. (Grace `warp <id>` already teleports because it goes through the
engine's LuaWarp, a different, complete path — use it meanwhile.)

## Live confirmation (RPC, this session)

- `mem_scan_f3 <pos> 0.05` → the only scale-1 copies of the triplet in the LP struct are `LP+0x6C0`
  and `LP+0x6C4` (plus a 0x2a0-strided render/anim matrix array elsewhere) — no separately-writable
  live-pos copy.
- `warp_xyz` readback == target immediately, `coords` reverts a beat later → physics reclaims +0x6C0.

## SetPos ABI — fully RE'd (2026-07-07, Ghidra decomp of the caller + the whole propagate chain)

Decompiled `SetPos` (`FUN_140dc6380`), its only caller (`er+0xda797b`, inside the big warp routine), the
propagate fn `FUN_140dc6e90`, the per-frame consumer `FUN_140dc6600`, and the terminal fns
`FUN_140dc7b40`/`FUN_140dc7260`/`FUN_140dc8150`. Result: a complete, safe call recipe — no remaining
unknowns.

**The ABI (MSVC x64):**
```
void SetPos(rcx = ChrIns*, rdx = PosStruct*, r8 = name-or-null, r9b = hardSet)
```
- `SetPos` itself: `or [rcx+0x160], 0x80` (arm the pending-teleport bit) → `movaps xmm0,[rdx+0x30]` →
  `movups [rcx+0x6c0], xmm0` (stage the 16-byte pos) → sets its own 5th arg to the `.rdata` const **-1.0f**
  → `call FUN_140dc6e90(rcx, rdx, r8, r9b, -1.0f)`. **It never touches r8/r9**, so those flow straight into
  the propagate fn — the caller MUST set them.
- **`rdx` (PosStruct) is read ONLY at `+0x30`** — a 16-byte xmmword `{x, y, z, w}`. The caller builds a
  0x40-byte transform (identity-ish rows at +0x00/+0x10/+0x20, pos at +0x30) but SetPos + the whole chain
  read only +0x30 (confirmed: `FUN_140dc7b40`, which gets `param_2`=rdx from the propagate fn, is actually a
  **1-arg** function — it ignores rdx and re-reads the pose from the ChrIns sub-objects). ⇒ a zeroed 0x40
  block with `{x,y,z,w}` at +0x30 is sufficient.
- **★ The 4th float `w` lands on `ChrIns+0x6CC` = YAW** (movups copies all 16 bytes; +0x6C0/6C4/6C8 = x/y/z,
  +0x6CC = facing yaw). So set `w = current yaw` to preserve facing (or a target yaw to also rotate).
- **`r8` (name) is OPTIONAL — NULL is safe.** The propagate fn `FUN_140dc6e90` does
  `name = &default; if (param_3) name = param_3;` then `swprintf("%s_%I64x", name, ChrIns)` for an SFX id —
  null → the empty-string default, harmless.
- **`r9b` (hardSet) = 1** matches the engine's own legit caller. It gates `FUN_140dc7260`, which finalizes
  the warp-request bits on `+0x160`. Use 1.

**Why the raw `+0x6C0` store snaps back (confirmed):** `+0x6C0` is an output mirror. `SetPos` stages it AND
arms `+0x160 |= 0x80`; the per-frame consumer `FUN_140dc6600` (`if ((ChrIns+0x160) < 0x80) return 0;`)
only then drives the physics body toward `+0x6C0`. A raw store sets neither the 0x80 bit nor registers the
warp, and the per-frame integrator (`er+0xdc761c`, `addss xmm0,[rcx+0x6c0]`) overwrites it next tick.

**AOB (entry, UNIQUE — offline scan = 1 hit):**
`48 83 EC 38 80 89 60 01 00 00 80 0F 28 42 30 0F 11 81 C0 06 00 00`. (Not pinned in the shipped code —
`teleport_coords` uses the body-write chain, not SetPos. Kept here for the record.)

## Implementation (2026-07-07) — `goblin::warp::teleport_coords` (SHIPPED + VERIFIED)

`src/goblin_warp.cpp`: resolves `posObj = *(*(LocalPlayer+0x190)+0x68)`, reads the current body Vec3
(`+0x70/74/78`) and tile-local pos (`LP+0x6C0/6C4/6C8`), computes `body_target = target - (tile - body)`,
and writes the three floats back — all in a noinline body under SEH (clang-cl elision guard). The
LocalPlayer-null check is the not-in-world / re-entrancy guard. Wired into the RPC `warp_local`/`warp_xyz`
verbs and the render-API `warp_to_world_xz` (vmap click-to-warp) — all three previously did the broken raw
`write_player_local_pos` (+0x6C0) store. The input frame is unchanged (tile-local, what the verbs already
compute), so their arithmetic is untouched.

## Live-verify — DONE end-to-end (2026-07-07, Windows/ERRv2.2.9.6, attach-RPC)

- Chain resolved live from `LocalPlayer=0x2684…9080`: `+0x190`→module `+0x68`→posObj, Vec3 `+0x70` =
  (-1.006, 3.547, -3.805); tile-local (`coords`) = (-1.01, 91.55, -75.80).
- **Mechanism** (`mem_write`): body X += 20 → `coords` world X 10750.99 → 10770.99, tile-local X -1.01 →
  18.99, HELD (no snap-back). Restored cleanly. Proved the write target + the 1:1 delta mapping.
- **Wired verb** (fresh DLL, `teleport_coords` on the body chain): `warp_xyz 10773 9143` → player moved
  +25 m east (world 10748.36 → 10773.00), HELD after 1.5 s (`[WARP] teleport_coords … -> moved`).
  `warp_local 0 91 -60` → landed at tile-local (0, 87.5, -60) (X/Z exact; Y settled to ground by gravity),
  HELD. Both paths confirmed in-game.

Residual (low, not blocking): a far cross-map target may land in unstreamed void (use `warp <graceId>` for
a full area-load); co-op is a local-sim change → guard on `coop::others_present()` if used in a session
(same caveat as the vmap freeze). Y writes set absolute height — a target under terrain will fall/clip.

Related: [[re_findings_playerpos]] (the READ chain, RESOLVED), `windows_grace_warppin_teleport_re_findings.md`.
