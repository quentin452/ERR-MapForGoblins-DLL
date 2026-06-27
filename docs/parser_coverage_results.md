# Parser Coverage Verification Results

This document records the results of the MSB and EMEVD parser coverage verification tool (`test_msbe_coverage`), designed to ensure the completeness of our binary parser by identifying unread byte ranges.

---

## Tool Overview
The coverage tool tracks every byte read by the helper functions (`rd32`, `rd64`, `rdf`, `rd_utf16`) in `msbe_parser.cpp`. Any bytes in the decompressed file that are never accessed by the parser are highlighted as **unread** (printed in **RED**). 

This helps developers verify:
1. Whether any important quest/loot data fields are being ignored.
2. The structure of padding, headers, and developer-only metadata in FromSoftware's binary formats.

---

## 1. EMEVD Parser Coverage Result
* **File Tested:** `/home/iamacat/Games/ERRv2.2.9.6/mod/event/m34_15_00_00.emevd.dcx`
* **Decompressed Size:** 2,352 bytes
* **Coverage:** **61.22%** (1,440 bytes read / 912 bytes unread)
* **Parsed Elements:** `Direct: 1`, `Event1200: 0`, `Setters: 1`, `BossFlags: 0`, `PerTile: 0`

### Unread Ranges Analysis:
1. **`[0x00 - 0x0f]` & `[0x30 - 0x77]` (EMEVD Header Fields):**
   * Contains file format magic (`EVD\0`), version information, and section offsets/counts. The parser only reads the offsets it needs (like event count at `0x10` and instruction table offset at `0x28`), leaving structural padding and metadata unread.
2. **`[0xa8 - 0xc7]`, `[0xd8 - 0xf7]`, etc. (Skipped Instructions):**
   * EMEVD files contain many gameplay scripting instructions (e.g., character movement, UI displays) that our quest/loot tracker does not care about. The parser only targets event initializations (e.g. `RunEvent` bank 2000), skipping the rest.
3. **`[0x86c - 0x92f]` (Linked File Paths):**
   * Stored at the end of the file are UTF-16 developer paths pointing to common macro files:
     `N:\GR\data\Param\event\common_func.emevd` and `common_macro.emevd`.
   * The parser has no need to resolve these external paths, so they remain unread.

---

## 2. MSB Parser Coverage Result
* **File Tested:** `/home/iamacat/Games/ERRv2.2.9.6/mod/map/MapStudio/m10_00_00_00.msb.dcx`
* **Decompressed Size:** 12,806,184 bytes (~12.8 MB)
* **Coverage:** Validated and Parsed successfully.
* **Parsed Elements:** `OK` (Treasures: 113, Assets: 7,685, Enemies: 309, Regions: 0)

### Unread Ranges Analysis:
1. **`[0x00 - 0x07]` & `[0x0c - 0x13]` (MSB Header):**
   * Magic bytes `MSB ` and section offsets.
2. **`[0x26f0 - 0x270f]` (Section Names):**
   * The section name `MODEL_PARAM_ST` (stored in UTF-16). The parser skips directly to the section data offsets rather than reading the string names.
3. **`[0x2748 - 0x27af]` (Developer SIB File Paths):**
   * Contains paths to original 3D models on the developer's workstation:
     `N:\GR\data\Model\map\m10_00_00_00\sib\m000004.sib`
   * The parser only reads the model name (`AEG...`) for loot mapping and ignores the raw SIB file path.

---

## How to Run the Coverage Tool
To run the tool on any map or event file:
```bash
cd tests
g++ -std=c++17 -DPARSER_COVERAGE -I../src -I../third_party test_msbe_coverage.cpp ../src/worldmap/msbe_parser.cpp ../src/stb_image_impl.cpp -o test_msbe_coverage
./test_msbe_coverage <path_to_file.msb.dcx / path_to_file.emevd.dcx>
```
