# Plan — Replace the FMG slot-walk with the native `GetMessage`, kill `#ifdef MFG_VANILLA`

**Status:** IMPLEMENTED + VISUALLY VERIFIED on ERR (`a64f4e1`, 2026-06-30). On-demand `lookup_text`
+ `decode_textid` landed; eager copies neutralized; sanitizer uses live validity. Log: `[SIG] PASS
GETMESSAGE` (unique), `GetMessage resolved at …d3c0` (= match−5), `setup_messages 11.26 ms`, no crash;
user confirmed labels render. **Remaining:** (1) cleanup commit — physically delete the dead collection
loop / copy lambdas+calls / both `#ifdef MFG_VANILLA` blocks (currently only neutralized); (2) verify the
vanilla+DLC profile (one-DLL-for-all is untested off ERR); (3) re-confirm EventTextForMap (600M/slot 34).
Build infra fixed in `fa75402` (libzstd.dll + tools/sitecustomize.py).

Prereqs already on master:
- `re_signatures.hpp` — `GETMESSAGE` AOB + health-check entry (interior anchor, `entry = match-5`).
- `docs/re/windows_native_msg_getter_re_findings.md` — the RE (FUN_14266d3c0, ERR hooks it, base
  slot is DLC-merged under ERR, DLC slots are 1-string stubs, GetMessage bounds-checks internally).

## Why this is the right end state

Loot/place names today are resolved in two phases inside `src/goblin_messages.cpp::setup_messages`:

1. **Collect** every name id referenced by markers (baked `MAP_ENTRIES` textIds + live
   `resolve_loot_item_textid`), bucketed by offset band into `*_ids_needed` sets.
2. **Copy** the strings out of the game's FMG slots into an **expanded PlaceName FMG** the mod
   builds and patches in (`g_expanded_placename_fmg`), via hand-walking the MsgRepository slot
   group tables (`copy_fmg_entries` / `copy_fmg_layered` targeted, `copy_fmg_all_layered`
   whole-namespace). Markers then read labels at render time through `lookup_text(id)` →
   `g_expanded_placename_fmg`.

That hand-walk is the whole problem: walking vanilla DLC slot numbers under ERR's loader is
**binder index/layout corruption (NOT an AV — `seh_call` can't catch it)** → the v1.0.15
`?PlaceName?` wipe. `#ifdef MFG_VANILLA` exists only to pin ERR to base-only slot lists.

The native `GetMessage(repo, 0, slot, msgId)` bounds- and null-checks internally and (under ERR)
returns base+DLC-merged content because **ERR hooks it**. So resolving names through `GetMessage`
instead of hand-walking:

- **cannot corrupt** — we never index a slot's internals; ERR's shifted/stub slots can't crash us;
- is **mod-agnostic** — ERR (merged base), vanilla (base + real DLC slots), Convergence (DLC layers)
  all resolve through the same call;
- deletes **both** `#ifdef MFG_VANILLA` blocks in `goblin_messages.cpp` + ~200 lines.

**Failure mode is bounded:** a wrong offset-band → slot mapping yields a *blank* label, never a
crash. That is why this is safe to land even though the exact crash mechanic was never fully
reproduced — the new path structurally removes the crash class.

## Design — on-demand resolution

Keep the expanded PlaceName FMG for what only the mod has (real PlaceName locations + injected
cluster / major-region / enemy-name labels). Stop copying game FMG strings into it. Resolve those
lazily in `lookup_text`.

### 1. Resolve the function pointer (init)

```cpp
using GetMessageFn = const wchar_t* (*)(void* repo, uint32_t group, uint32_t fmgId, uint32_t msgId);
static GetMessageFn g_get_message = nullptr;
// in setup_messages, after msg_repository is resolved:
g_get_message = modutils::scan<const wchar_t*(void*, uint32_t, uint32_t, uint32_t)>(
    { .aob = goblin::sig::GETMESSAGE, .offset = -5 });   // -5: AOB anchors interior; entry = match-5
```
(`modutils::scan` supports `.offset` per `src/modutils.cpp:99`. Fn-ptr pattern: `goblin_markers.cpp:67-101`.)

### 2. One decode table (single source of truth, mirrors `encode_live_item`)

`decode_textid(int32_t id) -> {const std::initializer_list<int>& slots, int32_t real_id}` —
the inverse of the collection buckets in `setup_messages` (`:456-477`) and `encode_live_item`
(`goblin_inject.cpp:1103`):

| Band (encoded id) | Family | real_id | Slot family (dlc02, dlc01, base) |
|---|---|---|---|
| `< 50M` | PlaceName (locations) | id | `{429, 329, 19}` |
| `[50M, 100M)` | WeaponName **ammo** (stored as-is) | id | `{410, 310, 11}` |
| `[100M, 200M)` | WeaponName | id-100M | `{410, 310, 11}` |
| `[200M, 300M)` | ProtectorName | id-200M | `{413, 313, 12}` |
| `[300M, 400M)` | AccessoryName | id-300M | `{416, 316, 13}` |
| `[400M, 500M)` | GemName (+ArtsName) | id-400M | `{422, 322, 35, 42}` |
| `[500M, 600M)` | GoodsName | id-500M | `{419, 319, 10}` |
| `[600M, 700M)` | EventTextForMap | id-600M | `{467, 367, 34}` (see ★) |
| `[700M, 800M)` | NpcName | id-700M | `{428, 328, 18}` |
| `[800M, 900M)` | ActionButtonText | id-800M | `{32}` base only (★★) |
| `[900M, 950M)` | TutorialTitle | id-900M | `{475, 375, 207}` |
| `[950M, 960M)` | BloodMsg | id-950M | `{461, 361, 2}` |
| `[1600M, 1700M)` | NpcName hi (9MMMMVVV) | id-700M | `{428, 328, 18}` |

★ **EventTextForMap (600M) is currently UNSUPPORTED** (`:780-785` TODO: thought to need a separate
menu bank). But slot 32 (ActionButtonText) proved the slot array is indexed by global BND id, so
`GetMessage(repo, 0, 34, real)` may resolve it directly — **TEST this; if it works, 600M labels
(e.g. "Closed with an imp's seal") light up for free.** If slot 34 returns null, leave 600M
unsupported (return null → caller shows nothing, same as today).

★★ **Do NOT add DLC ActionButton slots 365/465** — `:803-806` documents that poking them past
`count2` derailed downstream copies into `?PlaceName?`. ER already merges DLC ActionButton into
slot 32. Keep `{32}`.

### 3. `lookup_text` fallback + cache

```cpp
const wchar_t *goblin::lookup_text(int32_t id) {
    // (a) expanded PlaceName FMG first — locations + injected cluster/region/enemy labels
    if (const wchar_t* s = lookup_expanded(id)) return s;     // current body, renamed
    // (b) on-demand native resolution for item/name bands
    if (!g_get_message || !msg_repository || id <= 0) return nullptr;
    static std::unordered_map<int32_t, const wchar_t*> cache;  // wchar_t* is game-lifetime stable
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    auto [slots, real] = decode_textid(id);
    const wchar_t* hit = nullptr;
    for (int slot : slots) {                                   // first non-empty wins (dlc02→base)
        const wchar_t* s = g_get_message(msg_repository, 0, (uint32_t)slot, (uint32_t)real);
        if (s && s[0]) { hit = s; break; }
    }
    cache[id] = hit;                                           // cache misses too (avoid re-query)
    return hit;
}
```
- `lookup_text` is called **per-frame** for visible markers (`map_renderer.cpp:762/776/797/1044`,
  `goblin_overlay.cpp:2260`) — the cache makes it a hashmap hit after the first resolve.
- The expanded-FMG branch still serves the injected labels (cluster/region/enemy) and real
  locations, which are NOT in any game FMG, so they must stay eagerly built.

### 4. Delete the eager game-FMG copies + both ifdefs

Remove from `setup_messages`:
- the `*_ids_needed` collection loop (`:441-486`) — no longer needed (the MAP_ENTRIES walk fed it);
- `copy_fmg_entries`, `copy_fmg_layered`, `copy_fmg_all_layered` lambdas + every call
  (`:712-830`, `:755-759`, `:773-777`, `:808`, `:822`, `:829`);
- the **`#ifdef MFG_VANILLA` slot-list block** (`:697-709`) — gone (slots now live in `decode_textid`);
- the EventText "not supported" warn (`:783-785`) — folded into decode ★.

KEEP: the injected-label pushes (enemy names `:842-852`, cluster `:868-875`, major-region
`:880-888`) and `patch_fmg_in_memory` (`:890`) — the expanded FMG still exists for those.

### 5. Sanitizer — drop `g_placename_valid_ids`, use live validity (kills the 2nd ifdef)

`sanitize_injected_textids` (`:1007`) currently strips markers whose textId isn't in
`g_placename_valid_ids`, and the **`#ifdef MFG_VANILLA` at `:900-953`** walks DLC PlaceName slots
329/429 to whitelist DLC-zone ids. Replace both: a textId is valid iff `lookup_text(id)` (now with
the GetMessage fallback, including PlaceName DLC via `{429,329,19}`) returns non-null. Delete
`g_placename_valid_ids` + the `:900` walk + its `:287-296` population.

## Runtime verification (the part Linux can't do)

Build (clang-xwin cross-build still compiles it), deploy, then **on the live game**:

1. **ERR** — open the map; for a marker of each category confirm the label resolves (not blank, not
   `?PlaceName?`): goods, weapon, **ammo**, armor, talisman, ash-of-war, NPC, **boss** (900M enemy
   name), action-prompt (800M), tutorial/codex (900M), bloodmsg (950M), location (PlaceName).
   Confirm **DLC items** resolve (ERR's hook merges them into the base slot).
2. **EventTextForMap (600M)** — check a 600M marker ("Closed with an imp's seal"): does slot-34
   GetMessage now resolve it? Record yes/no (bonus feature if yes).
3. **Vanilla + DLC** — DLC-only item names AND DLC-zone PlaceName labels resolve via the real DLC
   slot fallthrough (not blank).
4. **Vanilla + DLC + randomizer** (liveLootLabels) — randomized loot tooltips resolve (the old
   whole-namespace preload's job, now on-demand).
5. **The Convergence** — DLC-layer added bosses/zones resolve; no `?PlaceName?`.
6. Grep logs for `?PlaceName?` / blank-label warnings; confirm `[SIG] GETMESSAGE PASS@…` resolves
   uniquely (health check). Confirm no crash in `setup_messages`.

If a whole category is blank → its decode band/slot row is wrong (fix the table, rebuild). If a
single id is blank → it genuinely has no string in that family (expected; caller shows nothing).

## After it lands

- Delete `#ifdef MFG_VANILLA` from `goblin_messages.cpp` (both blocks) — DONE by this refactor.
- The remaining `MFG_VANILLA` use is in `goblin_config_schema` (ERR-only feature gating) — that is
  the SEPARATE runtime-profile-detection chantier (hash `regulation.bin` → capabilities), not this.
- Update `docs/changelog.md` (fix: mod-agnostic name resolution; removed FMG slot-walk crash class)
  and `docs/memory/bugs/` (the `?PlaceName?` / MFG_VANILLA entry → resolved via native GetMessage).
- Reassess `copy_fmg_all_layered`'s `ammo_as_is` raw-ammo PlaceName entries — the decode handles
  ammo via the `[50M,100M)` band, so the raw all-copy is obsolete (confirm no consumer keys raw ammo
  ids by a different encoding first).

## Reference (current code)

`src/goblin_messages.cpp`: `setup_messages` `:398`, collection `:441-486`, `copy_fmg_entries`
`:516`, `copy_fmg_layered` `:586`, `copy_fmg_all_layered` `:625`, item all-copy `:755`, name
all-copy `:773`, EventText TODO `:780`, ActionButton `:808`, BloodMsg `:822`, Tutorial `:829`,
injected labels `:842/868/880`, patch `:890`, `#ifdef` PlaceName-DLC whitelist `:900`,
`lookup_text` `:1065`, sanitizer `:1007`. `src/goblin_inject.cpp`: `resolve_loot_item_textid`
`:4756`, `encode_live_item` `:1103`. `src/re_signatures.hpp`: `GETMESSAGE`, `MSG_REPOSITORY`.
