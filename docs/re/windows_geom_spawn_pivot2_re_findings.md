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

## Verdict / recommendation
**Pivot 2 is where ADD should go** — it is the streaming-native, name-driven path and avoids every wall the
direct-ctor route hit. But it is a **multi-step subsystem build**, not a quick primitive: resolve the reqMgr
singleton (Q1) + confirm request→collidable placement (Q2) + the name format (Q3), then a `spawn_asset
<name>` dev RPC that registers a request and lets the streamer place it. Recommend the next Windows/Ghidra
pass target Q1+Q4 (reqMgr singleton + the owning feature) since those unlock a live probe.

## Anchors
- registrar `FUN_1406a5080` er+0x6a5080; request-name builder `FUN_1406c7000` er+0x6c7000; request state
  machine `FUN_1406c6050` er+0x6c6050 (state arg; `req+0x4d` bits; `FUN_1406a6630` block-registry).
- consumer `FUN_140699670` er+0x699670 (caller `FUN_14069a9b0` er+0x69a9b0); managers `DAT_143d69ba8`,
  `DAT_143d69968`, `DAT_143d76060`; player `DAT_143d65f88` (+0x1e508 = LocalPlayer).
- RB-tree ops `FUN_14069e660`/`FUN_1406a0270` on `mgr+0x318` (comparator `PTR_142a81860`).
