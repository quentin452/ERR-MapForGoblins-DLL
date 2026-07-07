# Player-position WRITE (teleport) — why raw `LocalPlayer+0x6C0` writes don't move the player

**Status: root cause SOLVED (2026-07-07, Linux/Proton live + capstone scan); SetPos ABI fully RE'd +
implemented (2026-07-07, Windows/Ghidra). AWAITING LIVE VALIDATION on Linux/Proton — see "Live-verify".**
The engine-`SetPos` call is coded (`goblin::warp::teleport_coords`, `SETPOS` AOB) and wired into
`warp_local`/`warp_xyz` + the vmap click-to-warp; the last step is a deploy + boot to confirm it moves the
player without a crash.

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
`48 83 EC 38 80 89 60 01 00 00 80 0F 28 42 30 0F 11 81 C0 06 00 00` → pinned as `SETPOS` in
`src/re_signatures.hpp` (health-table registered).

## Implementation (2026-07-07) — `goblin::warp::teleport_coords`

`src/goblin_warp.cpp`: resolves `SETPOS` at init, builds a zeroed 0x40 PosStruct with `{x,y,z,yaw}` at
+0x30 (yaw read live from `get_player_facing_yaw` = `ChrIns+0x6CC`, preserving facing), and calls
`SetPos(LocalPlayer, &struct, nullptr, 1)` behind the same noinline-CALL + SEH shape as `to_grace`. The
LocalPlayer-null check doubles as the not-in-world / re-entrancy guard (like `to_grace`). Wired into the
RPC `warp_local`/`warp_xyz` verbs and the render-API `warp_to_world_xz` (vmap click-to-warp) — all three
previously did the broken raw `write_player_local_pos` store. Frame is unchanged (the +0x6C0 tile-local
Havok frame the read probe already uses), so the existing verb arithmetic is untouched.

## Live-verify (Linux/Proton — the one remaining step; a bad call = ~3-min reboot, so gate carefully)

1. Deploy the fresh DLL; boot in-world; `mfg_build`+`status` to confirm the new DLL loaded (the `[WARP]`
   boot line must show `SetPos=<nonzero>`; a null there = AOB drift → re-find `SETPOS`).
2. `coords` to read the current tile-local pos, then `warp_local <x> <y+2> <z>` (nudge up 2 m) → the player
   should MOVE and STAY (no snap-back), facing preserved. `coords` again confirms the new pos holds.
3. `warp_xyz <worldX> <worldZ>` for a short intra-region hop (a far target may land in unstreamed void —
   that's the streaming gate, not a SetPos failure; use `warp <graceId>` for cross-map).
4. Watch for: a fault (SEH → `FAULTED` in the log, no move — means the RPC-thread context is unsafe for
   SetPos; fallback = drain it on the main-update thread via the geom-spawn per-frame hook, like ADD-AEG),
   a physics desync/fall-through (target under terrain), or a co-op desync (same local-sim caveat as the
   vmap freeze — skip/guard when `coop::others_present()`). If clean, mark this doc SOLVED + changelog it.

Related: [[re_findings_playerpos]] (the READ chain, RESOLVED), `windows_grace_warppin_teleport_re_findings.md`.
