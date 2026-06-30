# Follow-ups: loot naming + DX (queued 2026-06-30)

Spun off while fixing the nameless-loot tooltip (`fix/noname-loot-label`). Not started; ordered by value.

> **#1 STATUS 2026-06-30 — RESOLVED/CLOSED.**
> - **Cause (a) AMMO — FIXED + merged** (`aa373eb`). Ammo (WeaponName id ≥50M) was preloaded UNSHIFTED but
>   markers encode it at +100M; `copy_fmg_all_layered` now emits both keys. `[NONAME]` dropped dozens → 5.
> - **Cause (b) the 5 remaining GOODS (240/310/375/401/9800) — NOT a bug; genuinely ERR-custom nameless.**
>   Tested walking the DLC GoodsName slots (`goods_slots {419,319,10}` on the ERR build): the names did
>   NOT resolve — so they're absent from base AND dlc01 AND dlc02 GoodsName, i.e. no FMG name at all
>   (ERR-custom param with no name string, or named only in regulation). "Unknown item" placeholder is
>   correct. **DO NOT re-enable DLC item-name slots on the ERR build:** doing so reintroduced the v1.0.15
>   `?PlaceName?` crash even WITH the per-slot `seh_call` guard (it's binder index/layout corruption from
>   the wrong slot numbers under ERR's loader, not an AV SEH can catch). Base-only (`{10}`) is required.
>   Disk-verify is blocked on Linux (ERR `item_dlc02.msgbnd.dcx` is DCX **KRAK/Oodle**). If ever revisited,
>   resolve these from the regulation/param side, NOT the FMG DLC slots.

## 1. Resolve real names for the [NONAME] markers (vanilla AMMO)

Most `[NONAME]` hits are **vanilla ammo** (arrows/bolts: id 50000000 "Arrow", 52030000, 64540000 …, cat 2,
encoded `+100M`). The name EXISTS in `data/items_database.json` but `lookup_text_utf8(150000000)` returns
empty at runtime — the known **ammo FMG gap** (`docs/re/loot_ammo_encoding_finding.md`). Currently masked
by the "Unknown item" placeholder. Real fix: make the FMG preload / lookup cover the ammo keys (the icon
already resolves from the same key, so the path is close). Enable `diag_loot_flags` → `[NONAME]` dump for
the full offender list (now logs cat + lot_backed for every nameless marker, incl. non-lot like Aeonia).

## 2. "Unknown" fallbacks for non-loot markers (bosses / entities / locations)

`marker_label` only placeholders LOOT now ("Unknown item"). A nameless boss/NPC/entity falls through to
location-only, and a marker with neither name nor location renders blank. Add category-aware fallbacks:
"Unknown boss" / "Unknown NPC" / "Unknown location" (the Marker carries `category`; map it to a noun).
Keep it opt-in-safe — only when the real string is genuinely empty.

## 3. Show the item's MOD/source in the tooltip (DX +)

Idea: tag each marker with where the item came from (vanilla vs the active mod / which mod layer) and show
it in the tooltip. Hard part: detecting origin — compare the item id against a vanilla id set, or read a
mod manifest. Nice-to-have, not load-bearing. Scope before building.

## 4. Real DWARF debug build (we're blind on crash signatures)

The shipped DLL has no debug symbols (only `.pdata`), so crash triage is manual disassembly (see the
`crash_320` rebuild-race hunt). A `-gdwarf-4` build failed because it pulled the debug CRT
(`libcmtd.lib` / `libcpmtd0.lib` not in the xwin SDK). Fix: force the NON-debug CRT under `/debug`
(e.g. `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` + `/nodefaultlib:libcmtd` or split-DWARF into a sidecar)
so `llvm-symbolizer` / `addr2line` can map crash RVAs → source. Big quality-of-life for future crashes.
