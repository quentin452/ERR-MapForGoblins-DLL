# ADD-a-geom pivot 2 (asset streaming-REQUEST path) — assessment

Answers the Linux agent's ask (075267b4): decomp `FUN_1406a5080` full flow + what consumes the request.
Ghidra `query.java` on `D:\ghidra_proj2\ER`, imagebase `0x140000000`, 2026-07-03. **Verdict: pivot 2 is the
more viable ADD path — it is streaming-thread-friendly (no standalone-call hang) and a real caller proves it
yields a tracked spawn from a NAME — but it is a subsystem, not a one-call primitive.**

## `FUN_1406a5080` (er+0x6a5080) — the asset-request REGISTRAR
```c
longlong ensure_asset_request(ReqMgr *mgr /*param_1*/, wchar_t *name /*param_2*/) {
    if (!mgr[0x185]) return 0;                         // gate flag
    res = *(mgr+8);  slot = *(res+0xb0);
    if (!slot) return 0;
    id = FUN_1406abfa0();  if (!id) return 0;          // allocate/derive a request id
    srcType = FUN_14062e700(&desc, *(u32*)(res+0x30), mgr);   // the same 8-byte geom FieldIns id
    req = FUN_1406c7000(&desc, name, slot);            // build the resource REQUEST (name "%s_%04d" + FD4 container)
    if (!req) { mgr[0x330] = id; }
    else {
        // insert `req` into the reqMgr's RB-tree of requests, keyed by id:
        ... walk tree root *(mgr+0x320) ...
        node = FUN_14069e660(mgr+0x318, &PTR_142a81860, &id, &out);  // RB insert
        FUN_1406a0270(mgr+0x318, ...);
        node[5] = req;                                 // store the request in the tree node
        FUN_1406c6050(req, 4);                          // set request STATE = 4 (queue for the streamer)
        if (mgr[0x6a5] && *(mgr+0x658)) thunk_FUN_1457da52e(req);  // kick/notify
    }
    return req;
}
```
So it **registers a resource request** (by name/id) into the request manager's RB-tree at `mgr+0x318` and
sets state 4. The streamer (which owns `mgr`) then services the request on its own thread. `FUN_1406c6050`
is the request state machine (it flips resident/loading bits at `req+0x4d`, calls `FUN_1406a6630` to register
into the block instance list — the SAME registry MOVE/ADD touch, and `req+0x90` vfunc on transition).

## The consumer — `FUN_140699670` (er+0x699670): a name → placed-asset spawn
`FUN_140699670` proves the path ends in a real, tracked placement:
- reads **LocalPlayer** (`DAT_143d65f88 /*WorldChrMan*/ + 0x1e508`) + camera/facing math → a **world
  position** (`local_1a8..`) via a spatial query `FUN_140c74c70(..,0x67,..)`.
- resolves a target asset: `FUN_14069a4c0`/`FUN_14069b530` → indices `iVar5/iVar6`; builds an id
  `local_274 = iVar6 + 10000000` and **formats its name** `FUN_1401dbd90(&id, &name)`.
- looks it up in `DAT_143d69ba8` (`FUN_1406d0cd0(mgr, &id)`), computes a position offset, then
  **`req = FUN_1406a5080(lVar8 /*reqMgr*/, name)`** and, on success, appends to a `std::list` at
  `param_1+0x30/0x38/0x40` (the "list<T> too long" assert = the tracked-spawns list).

⇒ **The asset-request path is a genuine runtime spawn mechanism**: give it a NAME (from an id), it requests
the asset and the streamer places+tracks it — and because the work is queued to the streamer's thread
(state 4 + service loop), calling the registrar does NOT hang the way the direct ctor builder did.

## Why this beats the direct-ctor route (pivot 2 > standalone ctor)
- **No standalone-call hang:** you register a request (fast, non-blocking) instead of running the
  EH-wrapped, streaming-welded descriptor builder on the wrong thread.
- **The engine builds the owned descriptor + CSMsbPartsGeom itself**, in its native context — sidesteps the
  "no cheap independent `param_4`" wall entirely.
- **Name/id-driven**, which fits our world-editor model (assets are already identified by AEG name /
  itemlot / textid elsewhere in the mod).

## Open questions before a `spawn_asset` probe (the remaining work)
1. **The reqMgr singleton** (`FUN_1406a5080`'s `param_1`): which manager owns the `+0x318` request tree +
   `+8` resource? Resolve its accessor/AOB (grep `rtti_index.txt` around the callers
   `FUN_1401dc870`/`FUN_1406d0040`/`FUN_140699d80` — likely a `CSWorldGeom*`/streaming manager). Needed so
   the DLL can get a live `mgr`.
2. **Does the request yield a COLLIDABLE geom instance** (a `CSWorldGeomDynamicIns` with Havok) or only a
   rendered model? Trace `FUN_1406c6050` state 4 → the service path that turns a resident request into an
   instance (does it reach `FUN_1406b9880`/`FUN_140b32880`?).
3. **Name format** for a chosen asset: `FUN_1406c7000` builds `"%s_%04d"` from a part name + index; confirm
   the exact string an existing AEG asset uses so we can request a KNOWN-resident model (renders for sure).
4. **What `FUN_140699670` actually is** (the mechanic that owns it) — if it's a dev/debug "spawn at target"
   or a gameplay summon, its setup is a ready-made template for our probe. Decompile its caller
   `FUN_14069a9b0` (er+0x69a9b0) to name the feature.

## Q1 + Q3 + Q4 — ANSWERED (2026-07-04, callers `FUN_1406d0040` / `FUN_140699d80` / `FUN_14069a9b0`)
- **Q1 — reqMgr singleton FOUND.** `FUN_140699d80` derives `FUN_1406a5080`'s `param_1` as
  **`*(DAT_143d69ba8 + 0x30)`** — so **`DAT_143d69ba8` (er+0x3d69ba8) is the FD4Singleton** (assert-guarded,
  `w:\...\FD4Singleton.h`) and the request manager is `[singleton+0x30]`. Resolve `DAT_143d69ba8` via the
  standard FD4Singleton accessor AOB (like `MSG_REPOSITORY`) so the DLL can get a live reqMgr. (The other
  caller `FUN_1406d0040` instead reuses a request slot `treeNode[5]` from a tree at its own `param_1+0x20` —
  a per-frame refresh path; the singleton route is the one to drive.)
- **Q3 — name format FOUND.** `FUN_1406d0040` builds the asset name with
  **`swprintf(L"AEG%03u_%03u", n/1000, n%1000)`** (`FUN_14018fa70`) from an asset number `n` — i.e. spawnable
  assets are `"AEG###_###"` (e.g. `AEG099_090`). The `FUN_140699670`/`d80` path instead formats an
  `id = base+10000000` via `FUN_1401dbd90`. So: **request an asset by its `AEG###_###` name.**
- **Q4 — owning feature.** `FUN_140699670`/`FUN_140699d80` are **periodic, player-proximity, raycast-driven
  AEG-asset STREAMERS**: per-frame steps (`FUN_140699170`→`FUN_14069a9b0`, `FUN_14069a550`→`d80`) accumulate
  timers (`ctrl+0x8/0xc/0x10`), raycast from LocalPlayer (`FUN_140c74c70(..,0x5d/0x67,..)` on
  `DAT_143d76060`), resolve a nearby AEG asset, `FUN_1406a5080`-request it, and track it in a `std::list`
  (`ctrl+0x78/0x80/0x88`). This is an ambient/proximity asset system (grass/props/gimmick streaming shape) —
  a ready template: it proves "given a name + a world position, the streamer spawns+tracks the asset."

**Nuance that scopes pivot 2:** this path streams assets **from a known-asset registry** (the request
manager's tree). It cleanly re-requests/positions an asset the manager already knows → **ideal for placing
copies of EXISTING AEG assets** (exactly the world-editor "duplicate this asset over there" case). Placing a
*truly arbitrary new* asset may need registering it into that tree first (what `FUN_1406a5080`'s insert into
`reqMgr+0x318` does). For the mod's actual need (clone/place existing assets) that's fine.

## Q2 — ANSWERED (2026-07-04): the request IS a real `CSWorldGeom` instance (not visual-only)
Traced the state-4 service (`FUN_1406c6050(req,4)` → `FUN_1406c9c30`/`FUN_1406c8750`/`FUN_1406e38c0`). The
`req` object has the **EXACT `CSWorldGeom` instance layout** — every deref matches the ctor findings:
`req[2]=+0x10` record, `req+3=+0x18` pose module, `req[4]=+0x20` transform, `req+6=+0x30` `CSMsbPartsGeom`,
`+0x4d`/`+0x264`/`+0x268` state flags, `+0x6f` render sub-object. So `FUN_1406c7000` **allocates a real geom
instance** (reconciles the earlier "just a name builder" downgrade — it builds the name AND the
instance/container), and `FUN_1406c6050` is that instance's per-frame **load/visibility state machine**:
- **`FUN_1406c9c30`** = the "should-be-loaded/visible" decision (LOD/region/flags off `rec+0x11c..0x11f/0x18e`).
- **`FUN_1406c8750`** = `inst+0x268|=1` then **`FUN_1406a6630(inst+0x10, inst)`** — the SAME block
  instance-registry the Dynamic ctor uses. ⇒ the request registers as a first-class block instance.
- **`FUN_1406e38c0`** (on `inst+0x6f`) = resolves a resource-backed object into `inst+0x10`, registers via
  `FUN_1406a6570`, and **applies a world matrix** (`FUN_1409f1320`) — i.e. builds the **scene/render node**.

This is the **same instance machinery a natively-streamed geom uses** (block registry + scene node + the
`+0x18/+0x20/+0x30` modules) — NOT a detached decoration. **Verdict: a serviced request yields a real
`CSWorldGeom` instance that renders; collision follows from the standard world-geom path** (the model's
resident collision loads with the `CSMsbPartsGeom` model, same as any streamed AEG). **Static confidence:
high for "real instance + render"; the only thing left for a live checkmark is whether Havok collision is
fully wired for a request-created instance vs a native-streamed one** — settle that in the live probe (walk
into the spawned asset), not statically.

⇒ **Pivot 2 is validated: it's a genuine placement path, not visual-only.** No static blockers remain.

## Verdict / recommendation
**Pivot 2 is the ADD route, and its STATIC RE is now COMPLETE (Q1–Q4 all answered).** It is the
streaming-native, name-driven path that avoids every wall the direct-ctor route hit, and Q2 proved it yields
a real `CSWorldGeom` instance (not visual-only). Recap of what's known:
- reqMgr singleton = `[DAT_143d69ba8+0x30]` (Q1); name = `"AEG%03u_%03u"` (Q3); feature = a proximity
  AEG-streamer template (Q4); request → real block-registered rendered instance (Q2).

**Remaining before a live `spawn_asset <AEGname>` probe (small):**
1. ~~Resolve the `DAT_143d69ba8` accessor AOB~~ **DONE (pyghidra byte-scan, 2026-07-04).** Candidate AOBs
   (each verified UNIQUE image-wide; `relative_offsets {{3,7}}` lifts the rip-disp → `&DAT_143d69ba8`; then
   `reqMgr = *(void**)(singleton + 0x30)`):
   ```
   GEOM_REQ_MGR (preferred, er+0x1dcc53):  48 8B 0D ?? ?? ?? ?? 48 85 C9 74 16 B8 01 00 00 00 48 8D 53
   backup (er+0x1dc930):                   48 8B 0D ?? ?? ?? ?? 48 85 C9 74 14 83 CB 02 89 5C 24 20 48
   ```
   (`mov rcx,[DAT_143d69ba8]; test rcx,rcx; jz …` — the singleton null-check idiom. RVAs are Ghidra-DB
   build; the Linux agent adds this to `re_signatures.hpp` + the `[SIG]` boot health-check to confirm it
   resolves on the deploy build, per AOB doctrine.)
2. **Live probe (Linux/Proton):** `spawn_asset <AEGname>` → get `reqMgr` → `FUN_1406a5080(reqMgr, L"AEG###_###")`
   near the player; confirm it renders, then walk into it to confirm collision (the one live checkmark Q2
   left open).
This is a **multi-step subsystem build** vs MOVE's one-vcall, but there are **no static unknowns left**.

## Anchors
- registrar `FUN_1406a5080` er+0x6a5080; request-name builder `FUN_1406c7000` er+0x6c7000; request state
  machine `FUN_1406c6050` er+0x6c6050 (state arg; `req+0x4d` bits; `FUN_1406a6630` block-registry).
- consumer `FUN_140699670` er+0x699670 (caller `FUN_14069a9b0` er+0x69a9b0); managers `DAT_143d69ba8`,
  `DAT_143d69968`, `DAT_143d76060`; player `DAT_143d65f88` (+0x1e508 = LocalPlayer).
- **reqMgr singleton `DAT_143d69ba8` (er+0x3d69ba8); request manager = `[DAT_143d69ba8+0x30]`** (= `FUN_1406a5080` param_1).
- name format `swprintf(L"AEG%03u_%03u", n/1000, n%1000)` via `FUN_14018fa70`; clean wrapper `FUN_1406d0040`
  er+0x6d0040 (`request AEG#n at pos`, but needs the asset pre-registered in its `param_1+0x20` tree).
- per-frame streamer steps `FUN_140699170` er+0x699170, `FUN_14069a550` er+0x69a550; raycast `FUN_140c74c70`
  on `DAT_143d76060` (query types 0x5d/0x67).
- RB-tree ops `FUN_14069e660`/`FUN_1406a0270` on `mgr+0x318` (comparator `PTR_142a81860`).
