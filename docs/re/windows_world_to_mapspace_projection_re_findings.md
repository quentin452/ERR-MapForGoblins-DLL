# RE findings — the game's runtime world→map-space projection (kill the baked LegacyConv)

Answers `docs/re/windows_world_to_mapspace_projection_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v62..v65`, output `out_v64.txt`/`out_v65.txt`).
App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Read-only. Builds on
`marker_to_mapspace_re_findings.md` (which first found `FUN_140876140` + the live
`WorldMapViewModel` converter array) and `world_map_projection_re_findings.md` (the
map-space→screen stage). **The result: every piece of our baked projection is the
engine's own per-page converter table, built live in the `WorldMapViewModel` ctor from
`WorldMapLegacyConvParamGroup` — so we can read/call it and delete the baked data.**

---

## 0. TL;DR — the whole pipeline is one engine table we can read live

The native map projects EVERY point with a small array of **converter entries** that live
**inline in `CS::WorldMapViewModel`** (vtable RVA `0x2ad82e0`):

```
converters   = WorldMapViewModel + 0xF8     (array, stride 0x30, 8 slots)
count        = WorldMapViewModel + 0x280     (u64; ctor sets 8)
page table   = (&DAT_142ad82f8)              (bytes [00 01 0a ...] = page id per slot)
```

Projection of one point = loop the converters, call **`FUN_140876140`** (RVA `0x876140`)
per entry; the first entry whose (post-legacy-conv) area matches wins, and **that entry
folds `WorldMapLegacyConvParam` itself** (its `+0x28` node) before applying a per-page
affine. So:

- **Q1 projection fn** → `FUN_140876140` (exact math in §2). Thin callable loop wrapper =
  `FUN_1408877d0` (§4).
- **Q2 does it consume LegacyConv live?** → **YES.** Each converter's `+0x28` is an
  `WorldMapLegacyConvParamGroup` node; when present `FUN_140876140` calls `FUN_1408775e0`
  to remap the packed area/grid id **and translate** the world pos before the affine. The
  group is loaded in the VM ctor `FUN_1408855b0` (§3). → **drop baked `LEGACY_CONV`.**
- **Q3 constants** → per-converter `scale/origin/bias` (overworld 60/61 = `scale 1`,
  `originX 7168`, `originZ 16384`, `bias 128/128` → our `−7040 / +16512`). DLC + base-UG
  are **other slots in the same array**, loaded when their map is open → **read live, no
  baked DLC eyeball.**
- **Q4 callable entry** → call `FUN_1408877d0(VM, &outMapXZ, &packedId, &worldLocal)` (or
  loop `FUN_140876140` yourself), on the live VM. Signatures + arg layout in §4.
- **Q5 page/group** → the matched slot index indexes `(&DAT_142ad82f8)` → page id
  `{0 overworld, 1 underground, 10 DLC}`, with an **area==`0x0c` (12) ⇒ underground**
  override (`FUN_140887870`). → **drop `marker_group_from`.**
- **m19 / Chapel-type no-conv areas** → no converter accepts them (no LegacyConv row, area
  byte matches nothing) → `FUN_140876140` returns 0 for all slots → **the game does not
  place them.** Confirmed structurally; gate them (matches the brief).

> **Brief lead correction:** the prompt's lead `FUN_140d82770` is the **stage-2 Scaleform
> extent-fit** (map-space→screen, `world_map_projection_re_findings.md` §2), NOT the
> world→map-space fn. The real fn is `FUN_140876140`. `FUN_140a82a80`/`FUN_140a832a0`
> (build/reconcile) only walk the point RB-tree + drive the Scaleform icon update — they
> carry no projection math. The projection lives entirely in the `0x876xxx`/`0x885-888xxx`
> `WorldMapViewModel` block.

---

## 1. The converter entry (0x30 bytes) — field layout

Pinned from the builder `FUN_140876100` (RVA `0x876100`) + the reader `FUN_140876140`:

```
off    type   field            notes
+0x08  u32    key              packed: +0x09 u8 gridZbase, +0x0A u8 gridXbase, +0x0B u8 area
+0x0C  f32    originX
+0x10  f32    (originY/unused)
+0x14  f32    originZ
+0x18  f32    biasX
+0x1C  f32    biasZ
+0x20  f32    scale
+0x28  ptr    legacyConvNode   WorldMapLegacyConvParamGroup node; +0x10 = RB-tree of conv rows.
                               0 = no legacy fold for this page.
```

Live overworld values (from `marker_to_mapspace_re_findings.md` CT dump, slots seen as
area 60 & 61): `scale 1.0, originX 7168, originZ 16384, biasX 128, biasZ 128`.

## 2. `FUN_140876140` — project ONE point through ONE converter (exact)

```c
// char FUN_140876140(Converter* c, Vec2* outMapXZ, u32* packedId, Vec3* worldLocal)
//   packedId : (area<<24)|(gridX<<16)|(gridZ<<8)|_     (MapId-style; low byte unused)
//   worldLocal: {posX, posY, posZ}  -- AREA-LOCAL pos (NOT gridX*256+pos; see below)
u32 id = *packedId;  float px = worldLocal[0], pz = worldLocal[2];
if (c->legacyConvNode /*+0x28*/) {              // legacy dungeon → fold onto overworld
    FUN_1408775e0(c->legacyConvNode, &id, &local_world, packedId);   // remaps id + translates pos
    // (local_world now holds the translated {x,z}; id holds the dst area/grid)
}
if ((id>>24 & 0xff) == c->area /*+0x0B*/) {     // area must match this converter
    float gx = (id>>16 & 0xff), gz = (id>>8 & 0xff);
    float worldX = px + (gx - c->gridXbase/*+0xA*/) * 256.0f;
    float worldZ = pz + (gz - c->gridZbase/*+0x9*/) * 256.0f;
    outMapXZ->x =  (worldX - c->originX/*+0xC*/) * c->scale/*+0x20*/ + c->biasX/*+0x18*/;
    outMapXZ->z = -(worldZ - c->originZ/*+0x14*/) * c->scale            + c->biasZ/*+0x1C*/;  // Z flipped
    return 1;                                    // success
}
return 0;                                         // area mismatch → try next converter
```

Overworld (slot area 60/61) reduces to our known affine: `mapX = worldX − 7040`,
`mapZ = −worldZ + 16512` (since `originX−bias = 7168−128 = 7040`,
`originZ+bias = 16384+128 = 16512`, `scale 1`).

**Important callability note:** `worldLocal` is **area-local** `posX/posY/posZ`; the engine
reconstructs the global tile via `(gridX − gridXbase)·256`. Pass the raw
`WorldMapPointParam.posX/posZ` + the grid numbers in `packedId` — do **not** pre-compute
`gridX·256+pos`. (The player-pos `+0x6C0` frame is exactly this area-local pos.)

### `FUN_1408775e0` — the live LegacyConv fold (RVA `0x8775e0`)

RB-tree lookup keyed by the packed id (`*(node+0x10)` tree, compare `row+0x1c` vs key). On
hit it writes the **dst** packed id (`*param_2 = row[4]`) and **translates** the world pos
by the conv base offset (`dstPos = srcPos + rowBaseOffset`, `row+0x24`/`row+0x2c`), then
`FUN_140877840` re-normalizes id vs grid. This is precisely the *base-point translation*
`marker_to_mapspace_re_findings.md` §2 prescribed (and which the mod's
`project_dungeon_row_to_overworld` only approximates — it substitutes dst grid directly and
drops the offset, the area-16 "wrong region" bug). The predicate `FUN_140660fe0` short-
circuits to identity for ids that don't need folding.

## 3. Where the converters come from — VM ctor `FUN_1408855b0` (RVA `0x8855b0`)

`*param_1 = CS::WorldMapViewModel::vftable`. The ctor:
- sets `VM+0x280 = 8` (count) and inits 8 slots at `VM+0xF8` stride `0x30`
  (`do FUN_1408881d0(...); +=0x30; while(--8)`);
- builds `local_3e8 = CS::WorldMapLegacyConvParamGroup::vftable` and wires it via
  `FUN_140876ce0(VM+0x18, &group)` → this is the **live `WorldMapLegacyConvParam` load**;
- fills converter entries via `FUN_140876100(VM+0xF8, …)` with `scale 0x3f800000 (1.0)`,
  `bias 0x4300000043000000 (128,128)`, and the group node as `+0x28`.

So the converter table (incl. its legacy-conv folding) is **regulation-driven, rebuilt per
VM** — it self-adapts to ERR/mods, which is exactly why reading it live beats baking.

## 4. Callable entries (deliverable #4)

| RVA | fn | role |
|---|---|---|
| `0x876140` | `FUN_140876140` | project 1 pt thru 1 converter (§2) — returns 1 + fills `outMapXZ` |
| `0x8877d0` | `FUN_1408877d0` | **clean loop wrapper**: `(VM, Vec2* out, u32* packedId, Vec3* worldLocal)` → loops all converters, returns 1 on first match |
| `0x886b10` | `FUN_140886b10` | loop variant that also yields page (`(&DAT_142ad82f8)[i]`) and stashes result at `VM+0xa0` |
| `0x876100` | `FUN_140876100` | converter builder (field layout, §1) |
| `0x8775e0` | `FUN_1408775e0` | live LegacyConv fold |
| `0x8855b0` | `FUN_1408855b0` | `WorldMapViewModel` ctor (writes vtable `0x2ad82e0`, builds converters) |

**Two ways to use it from our DLL** (the VM is reachable via the menu / a vtable scan of
`0x2ad82e0`, same pattern as the cursor scan):

- **Call live (preferred):** `FUN_1408877d0(VM, &outMapXZ, &packedId, &worldLocal)`. Build
  `packedId = (areaNo<<24)|(gridXNo<<16)|(gridZNo<<8)`, `worldLocal = {posX,posY,posZ}`.
  The game folds LegacyConv + applies the right per-page affine. For the page, loop
  `FUN_140876140` yourself and take `(&DAT_142ad82f8)[matchedIndex]` (§5).
- **Replicate (no call):** read the converter array live (`VM+0xF8`, count `VM+0x280`) and
  run the §2 math in C++; walk the `+0x28` RB-tree for legacy folds. Same result, no thread
  constraints. Either way **the −7040/+16512 affine, the DLC eyeball, and `LEGACY_CONV` all
  come from this live table.**

## 5. Page / group selection (deliverable #5 — kills `marker_group_from`)

In the loop (`FUN_140886b10`, `FUN_140886c40`, `FUN_140887870`):
```c
for (i=0; i < VM[+0x280]; i++)
    if (FUN_140876140(&conv[i], out, id, world) && i < 3) { page = (&DAT_142ad82f8)[i]; break; }
```
- **`(&DAT_142ad82f8)` (RVA `0x2ad82f8`, right after the VM vtable) = bytes `00 01 0a`** →
  page id per slot: **slot0→0 (overworld), slot1→1 (base underground), slot2→10 (DLC).**
  Guarded by `i < 3`.
- **Override (`FUN_140887870` @ `0x88799…`):** if matched page==0 **and** area byte==`0x0c`
  (12) ⇒ page = underground. (Overworld + base-underground share the overworld converter;
  the area byte 12 bumps it to the underground page — matches the "one conversion, two
  pages" result in `marker_to_mapspace_re_findings.md`.)

Draw a marker only when its computed page == the open page (`menu+0x151` / `DAT_143d6cfc3`,
`world_map_projection_re_findings.md` §3).

## 6. AOBs / handles

- projection `FUN_140876140` `0x876140`; loop wrapper `FUN_1408877d0` `0x8877d0`; page+loop
  `FUN_140886b10` `0x886b10`; builder `FUN_140876100` `0x876100`; legacy fold
  `FUN_1408775e0` `0x8775e0`; VM ctor `FUN_1408855b0` `0x8855b0` (2nd vtable writer
  `FUN_1408861c0` `0x8861c0`).
- `CS::WorldMapViewModel` vtable `0x142ad82e0`; converter array `VM+0xF8` stride `0x30`;
  count `VM+0x280`; page table `0x142ad82f8` = `[00 01 0a]`.
- Converter fields: §1. The math contract (§2) + the live table are the stable part;
  resolve fns by AOB, the VM by vtable scan — RVAs drift with VMProtect.

## 6b. Independent static verification (objdump on the shipped exe, 2026-06-22)

Disassembled `eldenring.exe` (Steam, App 2.6.2.0) directly (`objdump -d` @ VMA
`0x140876140`, .text off `0x600`) — `FUN_140876140` matches §1/§2 byte-for-byte:
`mov rcx,[rcx+0x28]`→`call 0x1408775e0` (legacy fold), `shr ecx,0x18; cmp cl,[rbx+0xb]`
(area `+0x0B`), `(gridX−[rbx+0xa])·256 + px − [rbx+0xc]` then `·[rbx+0x20] + [rbx+0x18]`
(scale `+0x20`, biasX `+0x18`, originX `+0x0C`); Z path negated via `xorps xmm0,[mask]`.
Constants read live: the `·256` literal `0x1429ce8b4` = `0x43800000` (256.0f), the Z mask
`0x14329f470` = `80000000`×4 (sign flip), and the page table `0x142ad82f8` = bytes
`00 01 0a` = `[overworld, underground, DLC]`. **Findings confirmed on the exact build;
the RVAs are plain readable code (not VMP-wrapped) at these addresses.**

## 7. Open / runtime-confirm (quentin runs — game not running during this RE)

1. **Read the live array per page.** With the overworld map open, dump `VM+0xF8` × `VM+0x280`
   (CT `MapForGoblins_converter_dump.CT`): confirm slot0/1 = the 60/61 constants and the
   `+0x28` legacy node is non-null. Then open **base underground (m12)** and the **DLC /
   Realm of Shadows** map and re-dump — capture the slot2 (DLC, page 10) `origin/bias/scale`
   that we never solved (the "eyeball"). These are the only missing constants and they're
   just *there* in the live array.
2. **Confirm slot↔area↔page.** The live CT saw two entries (areas 60, 61); the static page
   table is `[0,1,10]`. Confirm whether 60/61 are slots 0/1 (and how 61 maps to a page) vs a
   single overworld slot — read the matched index live for a known 60 grace and a known 61
   grace. (Doesn't block the math; only the page label.)
3. **Validate a worked example.** Feed a known Limgrave grace (`areaNo`, `gridX/Z`, `posX/Z`
   from `data/grace_position_index.json`) through `FUN_1408877d0` (or the §2 replica) →
   compare `outMapXZ` to the `−7040/+16512` baked result; then through stage-2/3 to the icon.
4. **Confirm Chapel (m19) is unplaced.** Inject an area-19 row; verify `FUN_1408877d0`
   returns 0 for it (no converter accepts) → gate, don't project to garbage.
```
