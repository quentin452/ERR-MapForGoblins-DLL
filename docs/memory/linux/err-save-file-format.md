---
name: err-save-file-format
description: ERR (Elden Ring Reforged) reads ER0000.err NOT ER0000.sl2; same BND4/sl2 format; SteamID + checksum offsets for save swapping/patching
metadata: 
  node_type: memory
  type: reference
---

ERR (Elden Ring Reforged) uses a **custom save file `ER0000.err`**, NOT vanilla
`ER0000.sl2`. The `.sl2` in the save dir is the vanilla file and is IGNORED by ERR
(renamed/separated so Steam Cloud can't clobber the modded save). When swapping/editing
saves for ERR, target **`ER0000.err`** — editing `.sl2` does nothing in-game. (Burned a
whole session patching `.sl2` before noticing the game wrote `ER0000.err` at the live
mtime and many `ER0000.err.bak-*` backups existed.)

**`.err` is the SAME format as `.sl2`** — BND4 magic, 28967888 bytes, identical internal
layout. So vanilla saves and sl2 tooling apply directly once renamed to `.err`.

**Save dir (Proton):**
`~/.local/share/Steam/steamapps/compatdata/1245620/pfx/drive_c/users/steamuser/AppData/Roaming/EldenRing/<SteamID64>/ER0000.err`
(folder name = SteamID64; "Application Data" is a symlink → AppData/Roaming).

**Dropping someone's save into ERR (verified working swap):**
The SteamID64 (8 bytes LE) is embedded in **EVERY character slot AND the global menu
section** — patching ONLY the global one → "Save data is corrupted" (learned the hard way).
Must patch ALL occurrences + recompute ALL affected MD5 checksums.

Save = BND4 with 11 USER_DATA sections, each prefixed by a 16-byte MD5 of its data:
- Slots 0-9 (characters): slot N starts at **0x300 + N*0x280010**; MD5 `[start:start+0x10]`
  over data `[start+0x10 : start+0x10+0x280000]` (0x280000 data).
- Slot 10 (USER_DATA_010, global menu + the canonical SteamID at **0x19003B4**): starts at
  **0x19003A0**; MD5 over `[0x19003B0 : 0x19003B0+0x60000]` (0x60000 data — NOTE smaller).

Procedure (verified: 11 steamid occurrences in a 100% save = 10 slots + 1 global):
1. Replace every 8-byte occurrence of the old SteamID with the local one (folder name /
   <SteamID64>).
2. Recompute MD5 for slots 0-9 (0x280000) and slot 10 (0x60000); write back.
3. Verify zero old-owner traces + all 11 checksums valid, then write to `ER0000.err`.

Python: replace `struct.pack('<Q',old)`→`struct.pack('<Q',new)` everywhere; per slot
`buf[s:s+0x10]=hashlib.md5(buf[s+0x10:s+0x10+SIZE]).digest()` with SIZE=0x280000 (slots) /
0x60000 (slot10).

Local user SteamID64 = **<SteamID64>**. The user's live ERR save is backed up at
`<downloads>/ER0000_err_backup_pre100.{bak,zip}`.

Context: this came up while trying to reveal the full DLC map for overlay-marker testing —
the in-game flag-writer route failed because map reveal is per-tile
`WorldMapPieceParam.openEventFlagId` (ERR-remapped), so a vanilla 100% save was the
pragmatic path. See [[overlay-rendered-markers]].
