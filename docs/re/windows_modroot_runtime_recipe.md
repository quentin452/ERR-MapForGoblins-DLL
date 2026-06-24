# Runtime recipe — confirm the mod data-root via the Dantelion virtual-FS (Option B)

Goal: at runtime on the live ERR process (RPM / Cheat Engine / an in-DLL hook), capture **where the
game actually reads `map/MapStudio/*.msb.dcx` from**, loader-agnostic (ME3 / ModEngine2 / UXM / native
`mod/`), so the disk-loot feature (`loot_from_disk_msb` / `loot_msb_dir`) can self-locate the maps with
zero config. Companion to the static RE below and to
`windows_regulation_modroot_anchor_re_prompt.md`.

## Static map (already RE'd, eldenring.exe imagebase 0x140000000)
ER opens **nothing** by raw OS path — everything goes through named **virtual-FS aliases** resolved by a
singleton manager. The MSB path is built as the virtual string `mapstudio:/<map>.msb`, resolved down a
chain to a real device root:
```
mapstudio:/X.msb → map:/mapstudio → game:/map → game → system:/ → <real OS root>
```
Key sites:
| what | RVA | note |
|---|---|---|
| **Alias/Device manager singleton slot** | `0x48464a8` (VA `0x1448464a8`) | getter `FUN_141f48b40`; AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84` + `relative_offsets {{3,7}}`. Populated early by `FUN_141f49f60`, stable for the session. Alias containers at mgr `+0x20/+0x48/+0x68/+0x88`. |
| **Alias SETTER** `FUN_141ed7af0(alias, target)` | `0x1ed7af0` | AOB `48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8`. **RCX = alias (wchar_t*)**, **RDX = target (wchar_t*)**. This is where `system → <root>`, `mapstudio → map:/mapstudio`, … get registered. |
| **MSB path builder** `FUN_140e043e0` | `0xe043e0` | emits `L"mapstudio:/%s.msb"` / `L"mapstudio_dlc2:/%s.msb"`. |
| **MSB path getter** `FUN_140720bb0` | `0x720bb0` | MapId → the virtual path string (singleton `DAT_143d86bd8`). |
| **CSFile resource singleton** | `0x3d5b0f8` (`DAT_143d5b0f8`) | open via vtable `+0x10`; **arg is an `FD4BasicHashString` (hashed name, NOT a readable path)**. |
| **EBL file manager singleton** | `0x3d5b0a0` (`DAT_143d5b0a0`) | `CSEblFileManager`, the encrypted-bdt layer. |
| **DLMOW override operators** (mod-overwrite) | vtables `ReadFileOperator 0x31492b0`, `OverWriteFileOperator 0x3149330` | `DLMOW` = "DL Mod-OverWrite" — ER's own `mod/`-folder override layer; the operator object carries the **resolved real path string**. |

Why a static read of `system` alone isn't enough under an *external* ME3: ME3/ModEngine register their
own overlay device / hook file opens rather than rewriting the `system` root, so the `system` string can
point at the vanilla install while the mod's loose maps live elsewhere. Hence: observe the **resolved
per-file path** at runtime.

---

## Method A — bp the alias SETTER (recommended; one-shot, readable, loader-agnostic)
Captures the whole alias chain INCLUDING `system → <real root>`, with plain wide-string args.

**Cheat Engine:**
1. Module base of `eldenring.exe` (ASLR) → `B`.
2. Set a breakpoint at `B + 0x1ed7af0` (or AOB-scan `48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8`).
3. On hit, log `RCX` and `RDX` as **UTF-16 strings** (Memory View → these pointers). Continue.
4. Reload a save / let the game boot fully so all 50 device slots register.
5. In the log, find the row `alias == "system"` → its `target` = the **real OS root** the loader mounted.
   Also note `mapstudio`, `map`, `game` to confirm the chain.

**RPM/Python (no CE):** AOB-scan for the entry, write an int3 or use a hardware bp via your existing RPM
harness (`D:\ghidra_scripts\*.py`), read RCX/RDX as `wchar_t*`. Or simpler: hook it from inside the DLL
(you already inline-hook WinAPI in `goblin_overlay.cpp`) and `spdlog` each `(alias, target)` pair.

**Outcome → wiring:** if `system` resolves to the mod/UXM dir → done, that's `loot_msb_dir`'s parent. If it
resolves to vanilla while the mod is elsewhere → Method B.

## Method B — observe the real per-file path (true mod-aware answer)
The path the game ACTUALLY opens for an MSB, after all overlay/redirection.

**B1 — CreateFileW hook (simplest, you already hook WinAPI):** in the DLL, hook `kernel32!CreateFileW`
(under Proton it's Wine's impl, same name), filter `lpFileName` containing `MapStudio` / `.msb`, `spdlog`
the full path, pass through to the original. ME3/ModEngine hook CreateFileW *below* their redirect, so by
the time it reaches the real call the path is already the **mod file**. This is the loader-agnostic
ground truth. (Likely also `NtCreateFile` if the engine bypasses kernel32 — add it if CreateFileW is
silent.)

**B2 — DLMOW operator bp (in-engine alternative):** bp a ctor of `DLMOW::ReadFileOperator`
(`0x227b130`/`0x22795b0`) or `OverWriteFileOperator` (`0x22798f0`); the operator object holds the resolved
real path string — dump it on hit for `*.msb` opens. Use if you'd rather stay inside the engine than hook
WinAPI.

## Method C — the `(*(CSFile+0x10))` bp you asked about (documented, but weaker)
In `FUN_140a864c0` the MSB load does `(**(code**)(*DAT_143d5b0f8 + 0x10))(CSFile, &FD4BasicHashString)`.
To bp it: read `vt = [B+0x3d5b0f8]`, then `openFn = [vt+0x10]`, bp `openFn`. **RDX = &FD4BasicHashString**.
⚠️ The arg is a **hashed** resource name (`FD4ResNameHashStringTraits`), not a readable path — you'd see
a hash + an embedded name fragment, not `…\m10_00_00_00.msb`. Prefer A/B for readable paths; use C only
to confirm the call actually fires for the maps you expect.

---

## Decision matrix (after running A + B)
| observation | meaning | feature wiring |
|---|---|---|
| `system` (Method A) == mod/UXM dir, and B1 paths are under it | single root, loader puts everything in the game dir | read `system` via the anchor `0x48464a8` → set `loot_msb_dir = <system>/map/MapStudio`. Drop the ancestor-walk. |
| `system` == vanilla, B1 paths point into a **different** mod dir | external ME3/ModEngine overlay | the mod root = parent of the B1 `.msb` path. Either (a) derive it from a B1-observed open at init, or (b) keep config/ancestor-walk for this case. |
| B1/B2 silent (no `.msb` CreateFileW) but maps still load | engine reads from a packed bdt via EBL, not loose files | loose-file disk-loot can't see it; stays on the resident-MSB or baked path for that profile. |

## Notes
- All RVAs are for the analyzed build (imagebase `0x140000000`); AOB-scan, don't hardcode VAs (ASLR).
- Anchor lift for the eventual code path: `slot = AOB(48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84) + rel{{3,7}}`
  → `mgr = *(B + slot)`; mgr is non-null after boot (`FUN_141f49f60`).
- Ghidra tooling used: `D:\ghidra_scripts\query.java` + `rtti_index.txt` (see [[ghidra-re-tooling]]).
