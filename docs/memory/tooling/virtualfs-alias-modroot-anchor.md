---
name: virtualfs-alias-modroot-anchor
description: "ER Dantelion virtual-FS alias anchor for auto-locating the mod's map dir (zero-config loot_msb_dir)"
metadata: 
  node_type: memory
  type: reference
---

RE'd 2026-06-24 (via [[ghidra-re-tooling]] query.java + rtti_index) for the disk-loot mod-root
auto-locate (the `windows_regulation_modroot_anchor_re_prompt.md` pivot). Full recipe:
`docs/re/windows_modroot_runtime_recipe.md`.

**ER opens nothing by raw OS path** — all via named virtual-FS aliases resolved by a singleton manager.
Chain: `mapstudio:/X.msb → map:/mapstudio → game:/map → game → system:/ → <real OS root>`.

Key sites (eldenring.exe imagebase 0x140000000, RVAs — AOB-scan, ASLR):
- **Alias/Device manager singleton slot = `0x48464a8`** (VA 0x1448464a8). Getter `FUN_141f48b40`,
  AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84` + `relative_offsets {{3,7}}`. Populated early by
  `FUN_141f49f60`, session-stable. Alias containers at mgr +0x20/+0x48/+0x68/+0x88.
- **Alias SETTER `FUN_141ed7af0(alias,target)` @ `0x1ed7af0`** — RCX=alias wchar*, RDX=target wchar*.
  bp it at boot → log `system → <real root>` (loader-agnostic, readable). The cleanest runtime confirm.
- MSB path builder `FUN_140e043e0` @ 0xe043e0 (`L"mapstudio:/%s.msb"`); MapId→path `FUN_140720bb0` @ 0x720bb0.
- CSFile singleton `DAT_143d5b0f8` @ 0x3d5b0f8 (open = vtable+0x10, **arg = FD4BasicHashString, hashed — not a readable path**). EBL mgr `DAT_143d5b0a0` @ 0x3d5b0a0.
- **DLMOW** (DL Mod-OverWrite) operators = ER's own mod-override layer: `ReadFileOperator` vtable 0x31492b0,
  `OverWriteFileOperator` 0x3149330 — the operator object carries the resolved real path.

**✅ LIVE-VALIDATED 2026-06-24 (RPM on running eldenring.exe, scripts `<ghidra_scripts>\live_alias_*.py`):**
anchor `[base+0x48464a8]` → manager (a static global @ er+0x48464c0) → **alias table @ mgr+0x48** fully
readable. Table = array of **0x60-byte entries**: key FD4-wstring {vtbl@+0, ptr/SSO@+0x08, len@+0x18,
cap@+0x20}, value FD4-wstring {vtbl@+0x30, ptr/SSO@+0x38, len@+0x48, cap@+0x50}; SSO when cap≤7 (inline at
the field), else ptr. header {vtbl, begin, end, cap} → iterate [begin,end) stride 0x60. Live values:
`system → "<game_dir>\"` (ABSOLUTE install root), `game → system:/`,
`mapstudio → data2:/map/mapstudio/`, `regulation → game:/`, `cap_mapstudio → capture:/mapstudio/`. The
`data0..data3`/`dlc1` are DEVICE names (resolved by separate device objects, not in this string table;
their trees mutate live). ⚠️ **CORRECTION — Option A is a REGRESSION for ERR/ME3, do NOT wire it as primary.** The running game was
ERR via ME3 (`me3_mod_host.dll` loaded; exe = vanilla Steam `eldenring.exe`; `MapForGoblins.dll` from
`<ERR pkg>/dll/offline/`). `system` = the **vanilla** Steam dir `<game_dir>\` with 1347
VANILLA loose maps; ERR's real maps (964 .msb.dcx) live in the ME3 package `<windows_downloads>\ERRv2.2.9.6...\mod\map\MapStudio\`,
served by ME3's FILE-LEVEL redirect (alias layer untouched). So reading `system` would load **vanilla loot
positions, not ERR's**. The DLL's live `[LOOTDISK]` log confirms it ALREADY reads the correct ERR maps via
the existing **ancestor-walk** (commit 14897b5) — because the DLL ships INSIDE the ERR package, so
walking DLL-folder ancestors + probing `mod/map/MapStudio` lands on ERR's maps. **Verdict: keep the
ancestor-walk as primary; the `system` anchor is at best a FALLBACK/cross-check.** Option A is correct only
for in-place/pure-UXM (maps loose under the install dir, no external mod package). The only fully
loader-agnostic source = **Option B** (observe the real opened path, e.g. CreateFileW filtered to
`.msb`/`mapstudio`). mgr+0x88 tree = per-block bnd content
aliases (menutpfbnd/tpfbnd@m60_0002/...), value layout differs from the string table.

**Verdict (A vs B):** reading the `system` alias = **Option A** (real root; better+simpler than the
ancestor-walk commit 14897b5, covers UXM/native-mod, but an *external* ME3 makes `system`=vanilla).
Full mod-aware = **Option B**: observe the resolved per-file path at runtime (hook CreateFileW/NtCreateFile
filtered to `.msb`/`MapStudio`, OR bp a DLMOW operator ctor). There is **NO single clean resolve vmethod**
to call — open bottoms out in CSFile/EBL streaming with hashed names → must confirm at runtime before
calling. See [[handoff-loot-from-real-files]] (the feature this feeds).
