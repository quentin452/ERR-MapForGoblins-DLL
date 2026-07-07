# Player-position WRITE (teleport) — why raw `LocalPlayer+0x6C0` writes don't move the player

**Status: root cause SOLVED (2026-07-07, Linux/Proton live + static Ghidra-equivalent capstone scan).**
Fix (engine SetPos call) is scoped but NOT implemented — see "Next".

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

## Next (fix — a proper engine SetPos, follow-up; engine-critical)

Implement `goblin::teleport_player(x,y,z)` that CALLS `er+0xdc6380` (AOB the prologue above) with
`rcx = LocalPlayer`, `rdx = &{…; xmmword pos @ +0x30}`. **Unknowns to RE first (crash risk if wrong):**
`r8` (= caller's `[rbp-0x80]`, a pointer — maybe a rotation/quaternion or null-tolerant) and `r9b=1`.
Trace the caller `er+0xda7960` upward to see what fills `[rbp-0x80]` / `[rbp-0x50]`, or find a simpler
coordinate-warp export (er_console_mod's `tp` uses one). Validate live behind a snapshot (one bad call
= ~3-min reboot). Until then `warp_local`/`warp_xyz` are documented as non-functional; `warp <graceId>`
is the working teleport.

Related: [[re_findings_playerpos]] (the READ chain, RESOLVED), `windows_grace_warppin_teleport_re_findings.md`.
