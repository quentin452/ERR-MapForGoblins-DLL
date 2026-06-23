# RE brief — suppress the native world-map pins (overlay becomes the sole icon source)

**Goal:** the ImGui overlay now draws all our markers; we want to STOP the game from drawing its
own world-map icons so there's no double-draw — BUT keep the player "you are here" dot, fog-of-war /
map-fragment reveal, the player's own placed map markers, and quest/objective beacons. Concretely:
empty (or selectively filter) the game's built world-map icon set after it's built, every map open.

App 2.6.2.0 / ERR 2.2.9.6. Static Ghidra + a quick in-game container check.

---

## What we already KNOW (from prior RE in this repo — verify, don't re-derive)

- **`FUN_140a82a80`** = the world-map icon BUILD function (`CSWorldMapPointMan` build; this=rcx
  PointMan, ctx=rdx). Already hookable: AOB `WORLDMAP_POINT_CTOR` (in re_signatures.hpp), unique,
  no RIP-relative bytes. We ALREADY hook it (`wm_build_detour` in goblin_inject.cpp, for live-refresh)
  — calling the trampoline rebuilds the set.
- **`FUN_140a832a0`** = reconcile. (`docs/re/marker_to_mapspace_re_findings.md`, `marker_affine_hook_re_findings.md`.)
- **`CSWorldMapPointMan`** instance static `0x143d6e9b0`; the **built-icon map is at `+0x398`**
  (node key @ node+0x20). `CSWorldMapPointIns` is only a small set (special/interactive points); the
  bulk of icons live in the `+0x398` map.
- The plan: in our `wm_build_detour`, after `g_wm_build_orig()` runs, EMPTY (or filter) the `+0x398`
  set so the game draws nothing of ours/theirs — the overlay is then the sole icon source.

## What we NEED (decompile + a container check)

1. **What's in `CSWorldMapPointMan+0x398`?** Decisive: does it hold the WorldMapPointParam-derived
   icons (graces + the category/boss/item pins — what we want gone) AND/OR these we want to KEEP:
   - the player **"you are here"** dot,
   - **fog-of-war / map-fragment** reveal state,
   - the player's **own placed map markers** (the custom beacon the player drops),
   - **quest / objective beacons** (guidance targets).
   For each of those 4 keep-items: is it in `+0x398`, or a SEPARATE system/manager (so clearing
   `+0x398` leaves it untouched)? Name the separate managers/fields if so.

2. **Graces** — confirm the lit/unlit grace pins ARE in `+0x398` (they come from BonfireWarpParam at
   runtime). We will draw ALL graces ourselves once native graces are gone, so we need to know
   clearing `+0x398` removes them.

3. **How to safely EMPTY `+0x398` from the build hook.** The container type (map/tree — key@node+0x20):
   give the field layout (count, root/head, node next/key/value) and the SAFE way to clear it from our
   detour — preferably **call the game's own clear/reset method** on it (give the fn + signature), or
   the exact "set count=0 + reset root" sequence that won't leak/crash. If the icons are re-read from
   `+0x398` later (reconcile `FUN_140a832a0`, or a render walk at `R15+0x398` per
   marker_affine_hook), confirm clearing post-build is enough or whether the render walk must also be
   neutralised.

4. **Selective filter (fallback / if keep-items share `+0x398`).** If the player markers / objective
   beacons live in the SAME `+0x398` map as graces+categories, give the per-entry discriminator
   (icon type / iconId / source param / a type byte on the node) so we can remove ONLY the
   WorldMapPointParam category+grace entries and keep the player/objective ones.

5. **Player dot + fog must survive.** Confirm the "you are here" dot and the fog/fragment reveal are
   driven by separate systems (not `+0x398`) so suppression doesn't blank them. (The player position
   is `[[WorldChrMan+0x1E508]+0x6C0]`, already solved — the dot is the game's own render of it.)

## Deliverable

- Answer to #1/#2/#5: a table of {grace, category pin, player dot, fog, player-placed marker,
  objective beacon} → in `+0x398`? / separate manager+field.
- #3: the clear recipe — ideally `clearFn(ptr = *(base+0x3D6E9B0) + 0x398)` with its AOB/RVA, or the
  safe manual field reset.
- #4 (if needed): the node discriminator to filter by.
- Note any AOB/RVA touched (we resolve by AOB; RVAs drift per patch).

## Why / what we do with it

In `wm_build_detour` (already installed): after the original build, clear/filter `+0x398` so no
native category/grace pin draws. Then on the overlay side we drop the grace dedup (currently we only
draw UNDISCOVERED graces and let the native pin show discovered ones — `discover_flag` gate in
map_renderer.cpp) and draw ALL graces. Result: the overlay is the sole world-map icon source, with
the player dot, fog, player markers, and objectives still native.
