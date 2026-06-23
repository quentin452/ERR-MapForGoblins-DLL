# Findings — global resident item/asset position structure

Companion to `windows_global_item_position_structure_re_prompt.md`. **Verdict: DOES NOT EXIST.**

---

## §1 decisive experiment — REFUTED: there is NO global resident position table

**One-line:** Elden Ring streams asset placement **per-tile from the on-disk MSB**; only loaded (and
not-yet-evicted) tiles have resident positions. No structure holds all map-item positions globally.
Baked loot position is therefore **mandatory** — a full live switch is impossible.

### What we found (the per-tile asset buffer)
While loaded in Nokron (m12_02), a heap structure (`MEM_PRIVATE`, alloc base `0x1C0916D0000`,
~16.5 MB across 15 regions) holds, per asset instance, a record of roughly:

```
[ name "AEG099_xxx_xxxx" ][ pad ][ pos: Z@+0x00, Y@+0x08, X@+0x10 (plain float32) ]   stride ~0x38
```

The control chest `AEG099_990_9000` resolves correctly: name → position
`(X 1383.854, Y -777.913, Z 1764.673)` = its baked `items_database` value. The buffer is
**double-buffered** (a byte-identical copy ~0x6800 later). This is the decompressed/parsed MSB
parts list for the resident block — exactly the streaming model, not a global registry.

### The decisive scan (external RPM, `D:\ghidra_scripts\global_pos_scan2.py`)
After teleporting FAR (Altus Plateau, Nokron unloaded), scanned **all ~3 GB of MEM_PRIVATE** with a
proximity matcher (find X, require Y and Z within ±0x80 bytes — the engine's own record layout) for
a loaded control + **22 chests from distinct far/unvisited tiles**:

| target | hits |
|---|---|
| CONTROL m12_02 (Nokron) | **7** |
| m10 (Stormveil), m11 (Leyndell), m12_05, m16 (Raya Lucaria), m21 (DLC), m30/m40 (dungeons), m61 (DLC), and 11 spread-out m60 overworld tiles | **0 each (22/22 absent)** |

Only the just-left tile is resident; every far/unvisited chest is absent across the entire private
heap. → **per-tile streaming, no global table.**

### Why the "it's global!" first impression was wrong (eviction lag)
The Nokron chest still read at Altus because its tile buffer **was not yet freed** after the
teleport. In Cheat Engine this showed as the target address staying **white** (page still mapped)
while many siblings went **red** (already-freed) — partial eviction in progress, i.e. a stale buffer,
not a persistent global one.

### On the encoding caveat (now largely closed)
The prompt warned a refute only rules out plain-float encodings. But we **found the engine's actual
representation** — plain float32 in MSB-local coords — and the control hits 7× in it, while far
chests are absent everywhere in that same representation. A separate global table in some exotic
(quantized/packed/AABB) encoding is implausible when the engine's own placement buffer is plain
float. The refute is strong.

### §2 candidate owners — not pursued (moot)
With §1 refuted, the §2 hunt (CSWorldGeomMan parent, streaming/tile manager, resident MSB cache,
geof save table, WorldMapPointParam item rows) is unnecessary — none can hold what doesn't exist
resident. The only resident position source remains `CSWorldGeomMan` = loaded tiles only
(windows_live_loot_position_re_findings.md), which is the per-tile buffer above.

---

## Implication (ties off the loot-position thread)

- **Baked loot position is mandatory and correct.** Live read covers only the loaded bubble; there
  is no global structure to switch to. Combined with the proven **0/30305 zero-drift**, baked is
  both faithful and the only map-wide option.
- **Future-proof** = re-run the offline position diff per ERR version bump
  (windows_live_loot_position_re_findings.md §0), NOT a runtime read.
- This permanently closes the global-position question. Tools:
  `D:\ghidra_scripts\global_pos_scan.py` (vec3) and `global_pos_scan2.py` (proximity/per-tile test).
