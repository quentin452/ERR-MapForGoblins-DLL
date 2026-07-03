# Windows-Ghidra + live-probe RE prompt — MSB placement WRITE (move/add a world object)

> **Volet A RESOLVED (static, 2026-07-03) → `docs/re/windows_msb_placement_write_re_findings.md`.** MSB
> position is snapshotted twice (CSMsbParts copies it out → CSWorldGeomIns gets a separate transform), so
> **resident-MSB byte writes are inert**; the movable transform is the FD4 location module at
> `CSWorldGeomIns+0x18` (matrix cached at `+0x44`). `CSWorldGeomDynamicIns` (`FUN_1406b9880`) is a movable
> geom class built on the factory `FUN_1406c5900` — the vehicle for move AND add. Next: find the transform
> setter vmethod (`query.java name:CSWorldGeomIns@CS@@`), then the live §C probe (re-scoped in the findings).


## Why (the strategic bet)
`docs/re/README.md` ranks **MSB WRITE as frontier #1** — the keystone that unblocks custom mob/treasure
placement (#5), new map content, and visions #1/#2/#3. We READ MSB fully (both routes proven:
`windows_resident_msbe_layout_re_findings.md`, disk + resident, one C++ parser `src/worldmap/msbe_parser.cpp`),
but there is NO write path. This sweep is the **cheap, decisive first probe** that tells us *at which layer
an edit actually moves an object in-game*, so we invest in the right write path instead of guessing.

**Core hypothesis to confirm/kill:** the MSB is parsed **once at tile-load** into spawned runtime instances
(`CSMsbPartsGeom` / `CSMsbPartsMap` / a FieldIns) that carry their **own** world transform; the resident MSB
blob is then just stale source data. If so, **writing the resident MSB `Parts.position` bytes post-load is
inert** — the object already spawned from a snapshot. The authoritative, movable transform lives on the
**instance**, not the MSB. We need to prove where that is and whether adding a NEW instance is reachable
without a full tile re-stream.

## What we already know (anchors — verify/extend, don't re-derive)
- **MSBE format + parser (DONE).** `msbe::parse_msb(buf,len,resident,blobBase,…)` handles disk
  (entry-relative offsets) and resident (absolute VAs). Resident `PARTS_PARAM_ST`: `+0x00 nameOffset(abs)`,
  **`+0x20 Vector3 position`** (block-local), rot `@+0x2c`, scale `@+0x38` (to confirm). Treasure event
  (type 4) → `{partIndex@+0x08, itemLotId@+0x10}`. Full layout in
  `windows_resident_msbe_layout_re_findings.md`.
- **Resident blob location (Python-only so far).** The decompressed MSB is resident and keyed by
  `CSMapbndFileCap` ("map:/m60…" @+0x18) / `CSMapbndResCap` (FD4ResRep, "m60…" @+0x18); a `"MSB "` magic scan
  also finds it. Path RE'd in `windows_runtime_msb_resident_re_findings.md`. **Not yet located from C++** —
  production loot uses the disk route (`resident=false`).
- **Instance-side footholds (reuse these).** Live entity transforms are partly RE'd:
  `windows_enemy_boss_runtime_pos_re_prompt.md` (enemy/boss live position), player pos
  (`windows_player_pos_RESOLVED_re_findings.md`), and the field-instance registry/pool
  (`windows_fieldins_pool_anchor_and_join_re_findings.md`, `windows_fieldins_registry_layout_and_preopen`).
  The World Editor already live-edits adjacent data (lot repoint / `lotItemId01` / `refresh_markers`).
- **World transform math (DONE).** Block-local → world = `gridXZ·256 + local` for the overworld
  ([[ghidra-worldmap-re]]); confirm per map type (m10 legacy vs m60/m80 grid).
- **Write primitive (DONE).** In-process store under SEH (`write_dw`/`write_bytes`) — **not** WPM-to-self,
  which silently fails on some pages (proven in the sidecar inventory strip). Use the in-proc store.

## Questions (Ghidra static on `D:\ghidra_proj2\ER`, imagebase 0x140000000; + one live probe)

### A. The MSB → instance load path (the crux — STATIC)
1. Find the loader that consumes `PARTS_PARAM_ST` and constructs the spawned part. RTTI anchors:
   `grep -iE 'CSMsbPartsGeom|CSMsbPartsMap|MsbPartsIns|CSWorldGeom' tools/ghidra/rtti_index.txt` → ctors →
   `query.java`. Decompile the ctor/loader and answer: **is the MSB position COPIED into the instance
   (snapshot) or does the instance hold a POINTER back into the resident MSB?** (Settles whether resident
   MSB writes are inert.)
2. On the spawned instance, find the **authoritative world transform** the render/physics use — the 4x3/4x4
   matrix or `{pos,rot,scale}` block, and its offset from the instance base. Cross-ref the enemy/boss and
   FieldIns transform findings: is it the same transform struct family (so we can reuse that write path)?
3. Trace how an instance is registered so it becomes visible/collidable: the FieldIns/WorldGeom registry
   join (already partly RE'd). What is the **minimum** an object must be linked into to render + collide?

### B. Reachability of an ADD (STATIC, scoping only — don't build it)
4. Is there a callable "spawn a Part/asset instance at a transform" entry (the load path factory) that
   could be driven directly, or is spawning only reachable via a full MSB parse at tile-stream? Identify the
   factory function + its arg shape (map id, model/asset name, transform). This scopes the difference
   between "move existing" (cheap) and "add new" (the real goal).
5. What triggers a tile **re-stream/re-parse** (so an edited disk/resident MSB would re-spawn at new data)?
   Find the CSFileStep/mapbnd (re)load entry — is walking out-of-range and back sufficient, or is there a
   forced-reload hook?

### C. Live decision probe (in-DLL dev RPC; runs on Linux/Proton where the game lives)
6. Add a throwaway dev RPC `msb_probe` that, in a streamed detail map (e.g. a camp with treasures):
   - **Target 1 — instance transform:** pick a known spawned part (via the FieldIns/geom registry), write a
     new position to the instance transform field from A2, and observe in-game. **Expected: it moves.**
     Confirms the transform-write primitive + the authoritative field.
   - **Target 2 — resident MSB bytes:** locate the resident blob (A/`"MSB "` scan), write a new
     `Parts[i].position` (+0x20), and observe. **Expected: nothing** (snapshot) — which confirms A1 and
     tells us MSB-write needs a re-stream (Q5) to take effect.
   Report which target moved the object. That single result picks the write path.

## Deliverable
`docs/re/windows_msb_placement_write_re_findings.md`:
- A1 verdict (snapshot vs pointer) + the instance ctor/loader address.
- A2 the authoritative instance transform offset + which existing transform-write path it reuses.
- The live-probe result (which target moved the object) → the recommended write layer for **move** now.
- B4/B5 scoping for **add** (spawn factory + re-stream trigger) → the follow-up RE, sized.
- If "move existing object" is proven, that alone is a shippable World-Editor slice (drag a placement live);
  note it for `docs/HANDOFF.md` + the World Editor next-slices list.

## Notes
- Static-first for A/B (no runtime needed to read the load path); the live probe (C) is a 10-min in-DLL test
  on the Proton box — gate it behind a dev RPC, never ship the write.
- Pin code SIGs, never raw RVAs (AOB doctrine, `common.md`); struct offsets are more patch-stable than RVAs.
- Keep it mod-agnostic: the load path + instance transform are engine-level (vanilla/ERR/ERTE/Convergence
  all share them) — do not anchor on ERR-specific asset names.
