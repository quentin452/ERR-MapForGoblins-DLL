# RE prompt — Native message getter (FMG id → merged string), to kill the FMG slot-walk + `#ifdef MFG_VANILLA`

**Platform: Windows (runtime RE — Ghidra + Cheat Engine + live game). Status: WAITING on RE.**

## Goal (one sentence)

Find and signature the game's **own** message-lookup function — given an FMG category + message id it returns the string already **merged across base / `_dlc01` / `_dlc02` layers** — so MapForGoblins can resolve loot/item/place names by calling the engine instead of hand-walking the `MsgRepositoryImp` slot array.

## Why this matters (the payoff)

The loot-name pipeline today is split in two:

- **Param side — already live & mod-agnostic (done):** marker `{lotId,lotType}` →
  `resolve_loot_item_textid()` (`src/goblin_inject.cpp:4756`) reads the LIVE `ItemLotParam`
  row (slot-1 item id `@+0x00`, category `@+0x20`) → `encode_live_item()`
  (`src/goblin_inject.cpp:1103`) offset-encodes it (goods `+500M`, weapon/ammo `+100M`,
  protector `+200M`, accessory `+300M`, gem `+400M`). No FMG touched. This yields the
  item's **name FMG id** (`real_fmg_id = key − offset_base`).
- **FMG side — the problem:** `setup_messages` (`src/goblin_messages.cpp`) then turns that
  name id into a STRING by **hand-walking the `MsgRepositoryImp` slot array** and copying
  group/string tables itself (`copy_fmg_entries` `:516`, `copy_fmg_layered` `:586`, manual
  nav `repo+0x08`/`repo+0x14` `:500-510`). A second hand-walk reader, `lookup_text()`
  (`src/goblin_messages.cpp:1079-1092`), reads a string by id the same way — the native
  getter replaces BOTH.

That hand-walk is why `#ifdef MFG_VANILLA` (`src/goblin_messages.cpp:697`) exists: the slot
numbers `{dlc02,dlc01,base}` are vanilla-correct but **ERR-wrong** (ERR's loader populates the
msgbnd differently → indices shift by a non-constant amount). Walking the vanilla DLC slot
numbers under ERR triggers the **v1.0.15 `?PlaceName?` crash** — and it is **binder
index/layout corruption, NOT an access violation**, so the per-slot `seh_call` guard
(`__try/__except`, catches AV only) does NOT stop it. So ERR is pinned to base-only `{10}`.

If we instead call the **engine's** getter (which internally maps the FMG category to the
correct slot for whatever mod is loaded and merges the DLC layers), then:

- no slot-number table, no per-mod list → **`#ifdef MFG_VANILLA` deleted**;
- DLC-layer strings resolve correctly on ERR **and** overhaul mods (Convergence) alike;
- we never index a slot ourselves → the binder-layout corruption class is **structurally
  impossible**;
- bonus: `copy_fmg_entries` + `copy_fmg_layered` + the header sanity guards (~100 lines) go away.

This is the single substitution that unblocks an `#ifdef`-free, one-DLL loot-name path. It is
independent of the runtime profile-detection work.

## What you already have to anchor from

- **`MsgRepositoryImp` singleton** is already signatured: `re_signatures.hpp:55` entry
  `MSG_REPOSITORY` = `"48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75"` (RIP-relative load,
  `offset_rel=3, offset_add=7`). Known live layout (from `src/goblin_messages.cpp:500-510`):
  - `repo+0x08` → `base_array` (pointer to the per-category array of FMG-group holders),
  - `repo+0x14` → `count2` (i32, number of categories/slots),
  - `base_array[0]` → `sub` = array of FMG pointers indexed by **slot** (PlaceName = slot 19;
    item-name slots base = goods 10 / weapon 11 / protector 12 / accessory 13 / gem 35,42 …).
- `from::params::get_param<>` infra already wired (`re_signatures.hpp` param-list sig,
  `src/goblin_legacy_fold.cpp:70`, `src/goblin_field_probe.cpp:261`) — reference for how a new
  sig is added/consumed if you need a param cross-check.

## The ask — find the engine message getter

In ELDEN RING the lookup is a method on `MsgRepositoryImp` (commonly
`CS::MsgRepositoryImp::LookupEntry` / "GetMessage"). Expected shape (CONFIRM exact arg count,
order, and meaning — do not assume):

```
wchar_t* GetMessage(MsgRepositoryImp* repo, uint32_t unk /*often 0*/,
                    uint32_t fmgCategory, int32_t msgId);
```

Find it and answer:

1. **Address / signature.** AOB pattern over a stable prologue, ER-version-independent enough
   to ship. Provide it in `re_signatures.hpp` style (pattern string + any `relative_offsets` /
   `disp_pos`/`disp_size` needed). Locate via xref to the `MSG_REPOSITORY` singleton: the
   getter is the function that loads the singleton (or takes it as arg) and indexes
   `base_array → sub[category]` then does the **layered merge** (tries `_dlc02`, then `_dlc01`,
   then base — the merge-at-lookup behavior described in `goblin_messages.cpp:579-585`).

2. **Arg semantics — the category arg is the crux.** Our hand-walk uses **slot indices**
   (10/11/12/19…). Does the native getter take that same slot index, or a different
   **FMG category enum/id** (e.g. a `WeaponName`/`GoodsName`/`PlaceName` enum)? Give the exact
   mapping from "the thing we want the name of" to the getter's category arg, for at least:
   GoodsName, WeaponName, ProtectorName, AccessoryName, GemName, PlaceName. If it is an enum,
   list the values for those categories.

3. **Return + miss behavior.** Returns `wchar_t*` (UTF-16) directly? What on a missing id —
   `nullptr`, empty string, or a placeholder? (We need to distinguish "no name" from "got a
   name" to fall back to the circle / drop the marker label.)

4. **Layer-merge confirmed.** Verify by live test: call it with a known **DLC-only** item id
   (a string that exists ONLY in `WeaponName_dlc01`, absent from base `WeaponName`) and confirm
   it returns the DLC string — i.e. the engine merges layers, so we never need the DLC slots.

5. **Thread/timing safety.** Safe to call from our build worker thread (same context as
   `resolve_loot_item_textid`)? Any requirement that the msgbnd be fully loaded first (we
   already gate on `world_map_param_ready()` / the MsgRepository poll)?

## Deliverable

- `re_signatures.hpp`-format signature for the getter (pattern + offset metadata), named e.g.
  `MSG_GET_MESSAGE`.
- The exact call convention: `wchar_t* get_message(uint32_t category, int32_t msgId)` wrapper —
  arg order, the `unk`/`0` arg if present, and the **category-id mapping** for the 6 FMG
  families above.
- Confirmation (live) that a DLC-only id resolves through it.

With that, `setup_messages` becomes: for each `*_ids_needed` id, `engine_get_message(category,
real_fmg_id)` and write into PlaceName@encoded_id — deleting `copy_fmg_entries`,
`copy_fmg_layered`, and the `#ifdef MFG_VANILLA` slot lists outright.

## Notes / constraints

- **Disk-verify is blocked on Linux** (ERR `item_dlc02.msgbnd.dcx` is DCX **KRAK/Oodle**), and
  this is runtime RE → **Windows is the right machine** (Ghidra + Cheat Engine + live game).
- Do **not** "fix" the slot numbers per-mod — that path is a per-mod dead-end and the
  corruption is non-catchable. The native getter is the mod-agnostic exit.
- Fallback if the getter resists signaturing: base-slot-only `{base}` as the universal list
  (kills the ifdef, but loses DLC-layer-only strings under overhaul mods — acceptable interim,
  == today's ERR/vanilla behavior).

Reference code: `src/goblin_messages.cpp` (`setup_messages`, `copy_fmg_entries`,
`copy_fmg_layered`, `#ifdef MFG_VANILLA` `:697`), `src/goblin_inject.cpp`
(`resolve_loot_item_textid` `:4756`, `encode_live_item` `:1103`), `src/re_signatures.hpp`
(`MSG_REPOSITORY`).
