# CE procedure — authoritative iconId offset per EquipParam (find-what-accesses)

Goal: read the COMPILED iconId byte-offset for each `EquipParam*` straight from the disassembly,
killing the per-param guessing. App 2.6.2.0 / ERR 2.2.9.6.

⚠️ **`0xC0` is NOT confirmed** (correction 2026-06-22): the `[CALIB]` density scan showed Weapon
`0xC0` has only `distinct=2` nonzero values over 6643 rows → it's a near-constant field, NOT iconId;
the earlier "Dagger row 1000 = 100 @ 0xC0, max=999" was a COINCIDENCE (some other field = 100 for
daggers). No heuristic (distinct, density) reliably finds iconId (low-cardinality + near-constant
fields trap the scorers + the pre-SOTE paramdef is ambiguous). **CE find-what-accesses is the only
ground truth.** Anchor on a LIVE value, not a guessed offset (below).

CE can't find a "paramdef" (none in memory — fields are compiled offsets). What it CAN read is the
exact `[reg+0xXX]` the menu uses when it draws an item icon. That `0xXX` IS the offset.

## 0. Setup (one launch)
- INI has `dump_icon_textures = true` (already on). Launch ERR.
- The DLL logs `[EQUIPADDR] <param> row_id=<id> base=0x<abs> stride=0x<structSize> u16@0x<guess>=<v> name='<name>'`
  for the first 8 rows of each param (`logs/MapForGoblins.log`). `base` = the row's absolute address,
  `stride` = struct size (the iconId offset is somewhere inside `[base, base+stride)`).
- Attach Cheat Engine to `eldenring.exe`. Load the Hexinton table too (for ItemGib).

## 1. Method (ALL params, incl. Weapon — value-anchored, no guessed offset)
Per param, two steps — **value-scan locates, find-what-accesses confirms + reads the exact offset**:

### 2a. Get the item's REAL iconId value
- Pick a logged row for that param (note its `base` + `name`). Own it (Hexinton ItemGib).
- Open the inventory and view ONLY that item → the DLL logs `[ICONMAP] 'MENU_FL_<N>_ptl' rect=…` —
  `N` is that item's true iconId. (Viewing one item at a time isolates which `MENU_FL_<N>` is it.)

### 2b. Value-scan the offset (no ownership needed for this part — the param row is always resident)
- CE → New Scan, **Value Type = 2 Bytes**, **Value = N**.
- Set the scan range: **Start = `base`**, **Stop = `base + stride`** (from the log). Scan.
- 1-3 hits. `offset = hitAddr − base`. If >1 hit, repeat 2a/2b with a SECOND item of the same param
  (different iconId, SAME offset) → the offset common to both = iconId. Discard coincidences.

### 2c. Confirm + read the compiled offset via find-what-accesses
- Right-click the surviving `base + offset` → **"Find out what accesses this address"**.
- Re-view that item so its icon redraws → an instruction fires:
  `movzx r32, word ptr [reg + 0xYY]`. **`0xYY` = the authoritative iconId offset for this param.**
  Firing exactly on icon-draw proves it's iconId (not a neighbouring field).

## 3. Record
Log the 5 offsets: Weapon `0xC0` (✓), Protector `iconIdM`/`iconIdF`, Accessory, Goods, Gem. These feed
the DLL's `verify_equip_iconids` table (replace the ✗ guesses) AND validate the self-calibrating
finder (`windows_live_paramdef_offset_re_findings.md` §3) — the calib heuristic should land on the
same offsets. Paste them back and we update the probe + memory.

## Notes
- The row is resident whether or not you own the item, so **value-scan (2b) works without owning**;
  only find-what-accesses (1, 2c) needs the game to DRAW the icon (→ own + view).
- Protector has TWO icon fields (`iconIdM` male / `iconIdF` female) — expect two adjacent u16 hits;
  both are valid, note both.
- If a value-scan gives 0 hits: the offset is outside `[base, base+stride)` (wrong row picked) or the
  iconId value `N` was misread — recheck the `[ICONMAP]` line for that exact item.
