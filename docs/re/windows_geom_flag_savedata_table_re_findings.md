# RE findings — GeomFlagSaveDataManager table layout (bound the scan + bulk-read)

Answers `windows_geom_flag_savedata_table_re_prompt.md`. Static RE on the
persisted Ghidra project `D:\ghidra_proj2\ER` (eldenring.exe, app 2.6.2.0,
imagebase `0x140000000`). Slot `GEOM_FLAG_SLOT` data addr = `0x143d69d18`
(RVA `0x3d69d18`).

## TL;DR

The `+0x08` table is **not** an unbounded `{u64,u64}` blob — it is a FromSoftware
**`DLFixedVector`** (Dantelion2; error strings name
`Dantelion2/Core/Util/DLFixedVector.inl`):

- **inline, contiguous**, fixed capacity **`0x189c` = 6300** elements,
- **element stride `0x10`**: `{ u32 tile_id; u32 pad; void* block; }`,
- **kept sorted ascending by `tile_id`** (binary-search / sorted-insert paths prove it),
- **live element count is an explicit `u64` field at `manager + 0x189d0`**.

So the `0x20000` ceiling, the stride-16 walk, and the 256-consecutive-empty
early-out are all replaceable by: read `count` at `+0x189d0`, then the valid
elements are **exactly `[0, count)`** — one bulk read of `count * 0x10` bytes at
`manager + 0x08`. Per-tile entry blocks are likewise contiguous (the scan's
"Layout A" is the real format), so one bulk read per tile.

Target: **`~16 000` RPM/refresh → `1 + 2·(#valid tiles)` (~tens).**

## Manager struct (`GeomFlagSaveDataManager`, base = `*0x143d69d18`)

| offset | type | meaning |
|---|---|---|
| `+0x00` | `void*` | vtable (FD4 component) |
| `+0x08` | `Element[6300]` | **inline `DLFixedVector` buffer** (sorted by `tile_id`) |
| `+0x189c8` | — | end of inline buffer (`0x08 + 6300*0x10`) |
| `+0x189d0` | `u64` | **element count (size)** — the real bound |

`Element` (stride `0x10`):

| offset | type | meaning |
|---|---|---|
| `+0x00` | `u32` | `tile_id` (sort key) = `area<<24 \| gridX<<16 \| gridZ<<8`, matches `encode_tile()` |
| `+0x04` | `u32` | padding |
| `+0x08` | `void*` | pointer to that tile's **entry block** |

Evidence:
- `FUN_1406ea190` (`lower_bound`): binary search, high bound = `*(u32*)(manager+0x189d0) - 1`, element addr = `base + idx*0x10`, compares `*param_2 < *(u32*)element` → **sorted by the u32 at element+0**.
- `FUN_1406e9810` (serialize-size): iterates `[base, base + count*0x10)` stride `0x10`, reads the block ptr at `element+8`.
- `FUN_1406e9be0` (sorted insert) and `FUN_1406e9f90` (erase-front/compact) both fault into `DLFixedVector.inl` with `count` at `+0x189c8` (relative to `base=manager+8`, i.e. `manager+0x189d0`) and the `0x189c` capacity cap.
- `FUN_1406e9a10` (grow/replace a tile's block): when `count == 0x189c` it frees `element[0].block` and shifts the vector down (`FUN_1406e9f90`), then allocates a fresh block and inserts in sorted position.

## Per-tile entry block (target of `element+0x08`)

Built by **`FUN_1406a9b80`** (serializes a *loaded* tile's geom-flag state). The
real header is the scan's **Layout A**:

| offset | type | meaning |
|---|---|---|
| `+0x00` | `u32` | tile coord/id (low) |
| `+0x04` | `u32` | tile coord/id (high) |
| `+0x08` | `u32` | **entry count** (number of records) |
| `+0x0C` | `u32` | aux id |
| `+0x10` | `Entry[count]` | **contiguous** entry array, **stride 8** |

`Entry` is one packed `u64`:
`record = ((geom_idx | (model_id << 17)) << 15) | present_byte`, which lands as:

| byte | scan field | meaning |
|---|---|---|
| `[0]` | — | present flag (`0x01`) |
| `[1]` | `entry_flags` | `(geom_idx & 1) << 7` → **always `0x00` or `0x80`** (this is *why* the scan keeps only those) |
| `[2..3]` | `geom_idx` | `geom_idx >> 1` (bits 16..31) |
| `[4..7]` | `model_hash` | **`model_id` (u32)** |

i.e. the full geom index = `(entry[2..3] << 1) | (entry[1] >> 7)` — exactly what
`aeg099_index_from_geof(geom_idx, flags)` reconstructs. **The existing decode in
`read_singleton_entries` is correct and must be preserved byte-for-byte.** The
"Layout B" branch in the current code is a guess that never actually fires; the
engine only ever writes Layout A.

## Contiguity / minimum RPM count

| level | contiguous? | reads |
|---|---|---|
| outer `DLFixedVector` | **yes** (inline array) | 1 (read `count` at `+0x189d0`, then `count*0x10` at `+0x08` — or one ~98 KB read covering both) |
| per-tile entry block | **yes** (count@+0x08, entries@+0x10 contiguous) | 2/tile (header `0x10`, then `count*8` entries) — or 1 if you over-read a bounded chunk |

`1 + 2·(#valid tiles)`. With tens of valid tiles → **~tens of RPM vs ~16 000.**

## Mutation model (item 4)

- A tile's block is **re-allocated** every time the tile transitions
  loaded→unloaded (`FUN_1406e9a10` allocates a new block, frees/replaces the
  old). **Block pointers are NOT stable across re-deactivations.**
- The outer vector is sorted-insert + erase-front-on-overflow, so **element
  positions also move** on insert/evict.
- ⇒ a "scan once, re-read known blocks" fast-path is **unsafe**. The correct
  cheap design is a **full bulk re-read each refresh** (it's already only
  `~tens` of RPM) plus the throttle below.

## Rewrite recipe (replaces the `0x08..0x20000` stride-16 scan)

In `read_singleton_entries` (`src/goblin_collected.cpp`):

```c
void *gf; if (!safe_read(slot, &gf, 8) || !gf) return;

uint64_t count = 0;
if (!safe_read((char*)gf + 0x189d0, &count, 8)) return;
if (count == 0 || count > 6300) return;            // sanity: capacity is 6300

// 1 bulk RPM of the whole live region of the inline vector
std::vector<uint8_t> tbl(count * 0x10);
if (!safe_read((char*)gf + 0x08, tbl.data(), tbl.size())) {
    // fallback: aligned 4 KB chunks, skip faulting pages
}

for (uint64_t i = 0; i < count; i++) {
    uint32_t tile_id = *(uint32_t*)(tbl.data() + i*0x10 + 0);
    uint64_t blk     = *(uint64_t*)(tbl.data() + i*0x10 + 8);
    uint8_t area = (tile_id >> 24) & 0xFF;
    if (area < 0x0A || area > 0x3D) continue;       // (now belt-and-suspenders, vector is dense)
    if (blk < 0x10000) continue;

    uint8_t hdr[16];
    if (!safe_read((void*)blk, hdr, 16)) continue;
    uint32_t ecount = *(uint32_t*)(hdr + 8);        // Layout A only — count @ +0x08
    if (ecount == 0 || ecount > 100000) continue;

    std::vector<uint8_t> ents(ecount * 8);          // 1 bulk RPM per tile
    if (!safe_read((void*)(blk + 0x10), ents.data(), ents.size())) continue;

    for (uint32_t e = 0; e < ecount; e++) {
        uint8_t *p = ents.data() + e*8;
        uint8_t flags = p[1];                       // unchanged decode
        uint16_t geom_idx = p[2] | (p[3] << 8);
        uint32_t model_hash = p[4] | (p[5]<<8) | (p[6]<<16) | (p[7]<<24);
        if (g_tracked_model_ids.count(model_hash) && (flags == 0x00 || flags == 0x80))
            out.push_back({tile_id, flags, geom_idx, model_hash});
    }
}
```

Notes:
- Keep **RPM** as the read primitive (clang-cl `__try` elision — see
  `clang-cl-seh-noinline`); we just issue far fewer, larger reads.
- Drop the Layout B branch, the `0x20000` ceiling, and the 256-empty early-out —
  all subsumed by the real `count`.
- Independent throttle win: `read_geof` need not run every 100 ms fast-phase
  tick; ~1 s is fine (collected graying isn't frame-critical).

## Validation (needs the running game — quentin)

Static RE only; the box had no live `eldenring.exe` at RE time. To confirm:
1. With the game running, resolve `gf = *0x143d69d18` (ASLR — use the AOB, not
   the static VA) and dump `*(u64*)(gf+0x189d0)` → expect a small count (tens..hundreds).
2. Diff the bulk-read collected set against the old stride-16 scan on the same
   save — must be **byte-identical**.
3. Confirm the rewrite is a no-op on Windows perf and kills the Linux/Proton
   `read_geof` cliff (re-run the `[BENCH]` aggregate; `refresh.collected.read_geof`
   should drop from ~153 ms to sub-ms).

## Function map (RVAs)

| RVA | role |
|---|---|
| `0x6ea190` | `lower_bound(key)` on the vector |
| `0x6e9be0` | sorted insert |
| `0x6e9f90` | erase-front / compact (overflow evict) |
| `0x6e9a10` | grow/replace a tile's block (alloc + sorted insert) |
| `0x6e9140` | per-tile insert on activate |
| `0x6e94a0` | per-tile remove/snapshot on deactivate |
| `0x6e9810` | serialize-size (walks `[0,count)`) |
| `0x6a9b80` | per-tile entry-block serializer (authoritative record layout) |
| `0x6d44e0` | GEOF save serializer (writes `"GEOF"` magic `0x...47454f46`) |
