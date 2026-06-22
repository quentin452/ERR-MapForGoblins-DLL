# RE prompt — GeomFlagSaveDataManager table layout (bound the collected-flag scan + enable bulk-read)

## Why this matters (Linux-only perf cliff)

`refresh.collected.read_geof` dominates the DLL's CPU budget **on Linux/Proton
only**. From a live 47 s session ([BENCH] aggregate report):

| label | calls | avg | max | %wall |
|---|---|---|---|---|
| refresh.collected.total | 115 | 163 ms | 793 ms | 39.9% |
| refresh.collected.read_geof | 115 | **153 ms** | 236 ms | **37.5%** |

On **native Windows the same code shows ~zero lag** (user-confirmed). That gap is
the whole story: the cost is **the number of `ReadProcessMemory` calls**, not the
work. RPM on `GetCurrentProcess()` is a cheap guarded-memcpy kernel path on
Windows (sub-µs), but under Wine each RPM is a **wineserver IPC round-trip**
(~10 µs). The current scan issues **~16 000 RPM calls per refresh** → ~150 ms on
Wine, ~nothing on Windows.

We use RPM (not a raw memcpy) deliberately: clang-cl elides `__try` around a raw
read, so RPM is our only non-elidable fault guard. The fix is therefore **not**
"stop using RPM" — it is "issue far fewer, larger RPMs" (bulk-read), which needs
us to know the table's real **bounds** and whether it is **contiguous**.

## What the code does today (the brute-force scan)

`src/goblin_collected.cpp` → `read_singleton_entries(slot, out)`:

```c
void *gf_ptr;                          // *(void**)slot  = GeomFlagSaveDataManager
for (int off = 0x08; off < 0x20000; off += 16) {   // 128 KB, stride 16 → up to 8192 iters
    safe_read(gf_ptr + off,     &id_val,  8);       // RPM #1  (tile_id in low 32 bits)
    safe_read(gf_ptr + off + 8, &ptr_val, 8);       // RPM #2  (ptr to that tile's entry block)
    if (id_val==0 && ptr_val==0) { if (++empty>256) break; continue; }
    uint8_t area = (id_val>>24)&0xFF;  if (area<0x0A||area>0x3D) continue;   // tile sanity
    if (ptr_val<0x10000 || ptr_val>0x7FFFFFFFFFFF) continue;                 // ptr sanity
    // per-tile entry block (layout A: count@+8 entries@+16 | layout B: count@+0 entries@+8):
    safe_read(ptr_val, header, 16);                 // RPM #3
    count = (countA in 1..99999) ? countA : countB;
    for (ei=0; ei<count; ei++)
        safe_read(entries_start + ei*8, entry, 8);  // RPM #4..  (one per entry, 8-byte stride)
    // entry: [1]=flags, [2..3]=geom_idx(u16), [4..7]=model_hash(u32)
    // kept when g_tracked_model_ids.count(model_hash) && (flags==0x00||0x80)
}
```

- The outer `for` treats the manager as **a flat array of `{u64 id, u64 ptr}`
  pairs starting at `+0x08`** — a HEURISTIC, not confirmed structure. The
  `0x20000` ceiling and `>256 consecutive empty` early-out are guesses.
- `GeomNonActiveBlockManager` is intentionally NOT scanned (different 0x820-byte
  layout — see `docs/geom_nonactive_block_manager.md` + `docs/geom_collection_tracking.md`).

### Resolve handles
- Manager pointer slot AOB (`GEOM_FLAG_SLOT`, src/re_signatures.hpp): `48 8B 3D ?? ?? ?? ?? 33 F6 48 85 FF 74 ?? 48 8B CF E8 ?? ?? ?? ?? 4C 8B 07` → `mov rdi,[rip+disp]` → the slot holds `GeomFlagSaveDataManager*`. (Was RVA `0x3D69D18`.)
- App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`.

## What we need answered

1. **What is the real container at `GeomFlagSaveDataManager + 0x08`?** A flat
   `(tile_id, block*)` array? a `std::vector`? a `std::map`/red-black tree? an
   open-addressed hash table? Give the exact type + the field offsets:
   - the **count / size** field (so we stop guessing `0x20000` + the 256-empty heuristic),
   - the **capacity / data-pointer** field if it's a vector/hashtable,
   - the **element stride** and the meaning of the `id` half (tile_id = `area<<24 | gridX<<16 | gridZ<<8`?).
   Decompile the function the AOB sits in (and the manager's ctor / the insert
   path that adds a tile) to recover the struct.

2. **Is the outer table CONTIGUOUS** (so one RPM over `[base+0x08, base+0x08+count*stride)`
   reads it all), or node-based (tree/bucketed → not bulk-readable in one shot)?
   If node-based, what's the smallest contiguous span we CAN bulk-read, and is
   there a flat index/array we can walk instead?

3. **Per-tile entry block** (the `ptr_val` target): confirm the real header — is
   it layout A (count@+0x8, entries@+0x10) or B (count@+0x0, entries@+0x8), or
   something else? Confirm entry **stride = 8** and the field layout
   (`[0]=?`, `[1]=flags`, `[2..3]=geom_idx u16`, `[4..7]=model_hash u32`). Is the
   entry array contiguous (bulk-readable per tile)? What do `flags` bits mean
   (we currently keep only `0x00`/`0x80`)?

4. **Mutation model** (for a possible incremental cache): when the player sets a
   geom flag, does the engine (a) append to an existing tile's entry array
   (realloc?), (b) add a new tile slot, or (c) rewrite in place? i.e. are the
   `block*` pointers stable across a session, or do they move on growth? This
   decides whether a "scan once, re-read known blocks" fast-path is safe vs a
   full bulk re-read each time.

## Deliverable

A short findings doc (`windows_geom_flag_savedata_table_re_findings.md`) giving:
- the struct(s): `GeomFlagSaveDataManager` (count/capacity/data offsets) + the
  per-tile entry block + the entry record, all as concrete offsets,
- whether each level is contiguous (→ how many RPMs a correct read needs at the
  minimum),
- the recipe to replace the `0x08..0x20000` stride-16 scan with: one bulk RPM of
  the bounded outer table → in-memory parse → one bulk RPM per valid tile block.
  Target: from ~16 000 RPM/refresh down to `1 + (#valid tiles)` (~tens).

## Notes for the implementer (post-RE)
- Keep RPM as the read primitive (clang-cl `__try` elision — see
  `clang-cl-seh-noinline`); just read in big chunks. If a bounded bulk RPM can
  still straddle an unmapped page, fall back to aligned 4 KB chunks, skipping
  faulting ones.
- A correctness-free win independent of this RE: throttle `read_geof` to ~1 s
  (collected graying is not frame-critical) instead of every 100 ms fast-phase tick.
- This is a Linux-only cliff; verify the rewrite is a no-op on Windows and that
  the collected-flag set is byte-identical to the old scan on the same save.
