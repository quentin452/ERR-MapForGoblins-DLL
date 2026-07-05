# hknpBodyCinfo field layout — naming prompt (Ghidra, Windows) — the last add_collision gap

**Goal:** name the `hknpBodyCinfo` fields so the Linux DLL can fill one correctly and spawn a walkable box
(`add_collision`, Route D — `add_collision_linux_impl_brief.md` §1, the ONE prereq before `add_box`). The
recipe + funcs are known (`hknpworld_addbody_slot_re_findings.md`); only the cinfo offsets are unpinned.
**Do NOT guess — a wrong poke crashes.** Live recon (Linux) already dumped the raw bytes (below); Ghidra just
needs to NAME the offsets.

## Decompile (D:\ghidra_proj2\ER, imagebase 0x140000000)
1. **`FUN_141911210`** — `hknpBodyCinfo` init (defaults). Its stores name the touched fields + struct size.
2. **`FUN_141878cf0`** — `hknpBoxShape` ctor. Confirm the **signature**: `(self?, const float halfExtents[4],
   float convexRadius)` — arg order, whether it takes a `this`/out ptr, what it RETURNS (the shape ptr we
   store into cinfo).
3. **`FUN_14167eb10`** + re-read **`FUN_1418a3080`** (the hknpCharacterProxy create+add template) — cross-map
   which cinfo offset each store targets: **shape**, **position** (vec3/vec4?), **orientation** (quat),
   **motionType** (STATIC enum value?), **collisionQuality/filterInfo**, and the **flags** offset that gets
   `|= 0x100`.
4. **`FUN_1418aabf0`** — `allocateBody(bodyMgr, hknpBodyId* out, hknpBodyCinfo* cinfo)`: confirm arg order +
   which cinfo offsets it reads (shape/motionType/position) — the ground truth for what MUST be set.

## Live recon data (ERR 2.2.9.6, from `add_collision recon`) — correlate against these
`hknpBodyCinfo` after `FUN_141911210` on a 0xCD-poisoned scratch (0xCD = field NOT written by init; 00 =
written-zero default). Struct spans ~0xA0:
```
+0x00: 00*14 cd cd            +0x10: 00*6 ff cd 00*8       +0x20: 00*9 cd*7
+0x30: 00*16                  +0x40: 00*14 [80 3f=1.0f @+0x4C]   +0x50: 00*16
+0x60: 00*16                  +0x70: [00 00 80 bf=-1.0f @+0x70] cd*4 00*8
+0x80: 00*8 ff ff cd cd ff ff ff 00   +0x90: ff ff ff 7f 00*12
```
Notable init'd defaults: `+0x4C = 1.0f`, `+0x70 = -1.0f`, `+0x88/+0x90 = 0xffff.. sentinels` (likely invalid
id/mask), a single `0xff` byte @ `+0x16`. The `+0x50..0x6F` block is all-zero → the likely pos/shape target
(brief hint: template writes "+0x50 pos/shape, +0x80 orientation, flags|=0x100").

A real **body[0]** header (0xb0 stride, for cross-ref — this is the BODY, not the cinfo, but shows the same
transform/shape/motion data the cinfo seeds):
```
+0x00: 3x rows [1,0,0,0][0,0,1,0][0,0,0,1?] = a 3x4 transform (rotation)   // +0x00=1.0 +0x16=1.0 +0x28=1.0
+0x30: 56.0, -88.0, -88.0, 0    // an AABB or a scaled position?
+0x40: int 1, 0x106, 0x28       // ids/type
+0x50: -0.74, 14.92, -2.96, 14.95   // a position (x,y,z) + w? (near a plausible world spot)
+0x60: ptr 0x1b66ea200          // <-- SHAPE ptr (heap) — confirm it's an hknp*Shape
+0x88: 1.0f   ...
```

## Deliver (into hknpworld_addbody_slot_re_findings.md)
A `hknpBodyCinfo` offset table: `shape`, `position`, `orientation`, `motionType` (+ the STATIC value),
`flags` (+ the 0x100 bit meaning), `collisionFilterInfo`/`qualityId` if `allocateBody` requires them — and
the `hknpBoxShape` ctor signature + return. That unblocks the Linux `add_box` + the `hf_probe` smoke test.
