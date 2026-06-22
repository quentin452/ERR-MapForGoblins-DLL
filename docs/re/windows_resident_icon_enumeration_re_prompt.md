# RE brief — enumerate ALL resident item-icon sprites at runtime (deliverable: a Cheat Engine table + offset recipe)

App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. ERSC + ERR; our DLL (MapForGoblins) is in-process
on Linux/Proton. **Stop guessing offsets statically — we need a runtime-validated recipe + a Cheat
Engine `.CT`** (same approach that cracked the player-pos yellow-dot). Read-only.

## Goal

Draw real ER item icons on our overlay map markers. We already copy a single icon's sprite rect
GPU→GPU once we know its sheet+rect+format. We can harvest icons **as the engine draws them** (works,
see below), but we want to **enumerate ALL currently-resident item icons in one pass** (proactive),
so markers show icons without the player hovering each item.

## What ALREADY works (don't re-derive)

- **Find-by-name hook = reliable.** We hook `FUN_140d63c30` (`er+0xd63c30`), signature
  `void* find(void* repo, void** out, const wchar_t* key)`. For every `MENU_ItemIcon_<id>` the engine
  resolves, we read the resulting `CS::CSTextureImage` and cache it:
  - `img+0x74/+0x78/+0x7c/+0x80` = sub-rect x0/y0/x1/y1
  - `img+0x10` → Render::Texture, `+0x70` → `ID3D12Resource` (the BCn sheet)
  - sheet `res+0x30` = DXGI_FORMAT (vkd3d internal)
  - `img` vtable == `er+0x2bb8910` (CSTextureImage) — our validity guard
  This yields correct icons (verified in-game: 139 hits on one inventory open, icons render). **`repo`
  (arg0) is stashed live as `g_icon_repo`.**

## What FAILED (the §8 walk — wrong target on this build)

A prior findings doc (`windows_runtime_item_sprite_re_findings.md` §8) said: enumerate via the loaded
**movie**: `movie+0x40 → res`, gate `res+0x88 == 4` (image-list), `res+0x90 → list`, `list+0x78`
count, `list+0x80` arr (stride 8), entry name DLWString `+0x18` (heap iff `*(entry+0x30) >= 8`). We
hooked the enumerate `FUN_140d69640` (`er+0xd69640`) to capture the movie (arg0) and walked it. Live
result (our `[WALK-DIAG]`, RPM reads):
- `FUN_140d69640` fires for **LOAD-screen movies** (`MENU_Load_*`), **count=1** — NOT the inventory
  item movie. The inventory icons never came through this enumerate.
- `res+0x88` reads **0x44**, not 4 → the static type-gate does not hold at runtime.
So either `FUN_140d69640` is the wrong enumerate for item icons, or the movie/offsets differ on this
build. The item icons clearly flow through the **find path** (`FUN_140d63c30` + its `repo`), not this
movie enumerate.

## What we need (runtime-validated)

**Primary: the resource REPOSITORY container.** `repo` (the `FUN_140d63c30` arg0 we already hold) is
almost certainly an `FD4ResRepository`/`FD4ResCapHolder` whose internal hash/table holds **every
loaded resource ResCap**, including all resident `MENU_ItemIcon_<id>` texture entries. Reverse its
container so we can **walk all loaded icon ResCaps** in one pass:
1. The repo's container layout: bucket array ptr + count/capacity offsets, the ResCap node link
   layout, and where each node's **name** (DLWString) lives.
2. Per ResCap: the path to its `CS::CSTextureImage` (or the sheet `ID3D12Resource` + sub-rect +
   DXGI_FORMAT) **directly**, so we don't have to re-resolve by name. If a direct path doesn't exist,
   confirm that calling `FUN_140d63c30(repo, &out, name)` for each enumerated name is the intended
   (and resident-safe) way.
3. How to filter to icon entries (name prefix `MENU_ItemIcon_`, or a resource-type tag on the node).

**Secondary (nice to have): force-load.** Even a full repo walk only yields icons whose sheets are
currently RESIDENT. If there's a cheap, safe way to make the engine load an icon sheet by id/name
(so we can harvest icons for items the player doesn't own), report the fn + args + safety constraints.
(Lower priority — resident-only is already useful.)

**Also confirm:** is `FUN_140d69640` ever the item-movie enumerate (and we just sampled load movies),
or is it purely load-screen? If there IS an inventory-movie enumerate with the §8 image-list, give its
fn + the corrected `res`/list/entry offsets for THIS build. (Repo-walk is preferred if it covers all.)

## Deliverables

1. **A Cheat Engine `.CT`** that, with the inventory open, resolves the repo from a stable anchor
   (AOB / the `FUN_140d63c30` callsite / a static) and shows the enumerated `MENU_ItemIcon_*` entries
   (name + sheet resource + rect + format) live — so we can watch it populate and validate counts.
2. A findings doc with the **stable offset recipe**: repo anchor (AOB/RVA), container walk (bucket/
   count/node/name offsets), ResCap→image path (or "re-resolve by name"), and the icon filter. RVAs
   drift under VMProtect — give the chain shape + offsets, resolve fns by AOB.
3. Validation notes: with N item icons resident, the walk yields N `MENU_ItemIcon_*` entries; spot-
   check a few ids' rect/sheet/format against the find-hook values (which are known-correct).

## Handles / leads
- find fn `FUN_140d63c30` `er+0xd63c30` (`repo`, `void** out`, `const wchar_t* key`) — we hold `repo`.
- CSTextureImage vtable `er+0x2bb8910`; rect `img+0x74..0x80`; sheet `img+0x10→+0x70`; format `res+0x30`.
- enumerate `FUN_140d69640` `er+0xd69640` (fires for load movies on this build — verify scope).
- create-image `FUN_140d6bbc0` (`WORLDMAP_CREATE_IMAGE` AOB) — the GFx image builder, if useful.
- names are GFx symbols `MENU_ItemIcon_<id>` (also `MENU_FL_<id>`, `KG_*`, `SB_ERR_*` for other art).
- our probe to extend: `goblin_inject.cpp` `find_detour` / `harvest_resident_icons` (RPM-safe, config
  `dump_icon_textures`). Point a repo-walk here once the layout is known.
- Constraints: RPM/WPM-safe, no `__try` reliance (clang-cl elides it — see the project SEH note).
