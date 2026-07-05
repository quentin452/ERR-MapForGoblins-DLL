# FromSoft debug-render / wireframe flag — RE findings (static, Ghidra, 2026-07-05)

Status: **static recon done → QUALIFIED GO for a COLLISION debug-draw; NO simple whole-scene restyle flag
found.** The expensive half of the GO/NO-GO — "does retail ER ship a live debug-draw path?" — is answered
**YES**: the debug-draw step tree and the Havok debug-display handler are compiled in AND constructed at boot.
The exact enable flag's value→visual-effect is the live-probe acceptance (flip + screenshot), handed to the
game-driving side. Imagebase `0x140000000`; tool `query.java` on `D:\ghidra_proj2\ER`. Answers
`windows_debug_render_flag_re_prompt.md`.

## ✅ Debug-draw infrastructure IS present and LIVE in retail
`FUN_140ded060` (the **`CSSystemStep` ctor**) builds the per-frame step tree and installs, as live child
steps (retail, not stripped):
- **`EzChildStep<CSDbgDispStep>` @ child slot `param_1[0x47]`** — the per-frame **debug DISPLAY step**.
- **`EzChildStep<CSDbgMenuStep>` @ `param_1[0x1b]`** — the **debug MENU step** (FromSoft's retained dev menu).
- `EzChildStep<CSDbgIdNameStep>` @ `param_1[0x4f]`, `EzChildStep<DbgRemoteStep>` @ `param_1[0x43]`.

So the debug-draw machinery runs every frame; it just early-outs unless its enable/mask is set. RTTI sweep
(`rtti_index.txt`) also confirms a full debug ecosystem: `CSDbgDispStep`, `CSDbgMenuStep`, `CSDbgIdNameStep`,
`CSDrawStep`, `CSFfxDebugDrawer`, `CSDbgDistMeasure`, `CSDbgEvent`, `CSDbgSignboardMng`, and the
`FD4DebugMenuModeChange*` pad handlers.

## ★ The two concrete draw paths + their gates

### 1. Havok COLLISION debug draw — `CSHkDebugDisp` = `hkDebugDisplayHandler` (the strongest GO)
- RTTI `.?AVCSHkDebugDisp@CS@@` vtable **er+0x2b92230**; ctor **`FUN_1452701f3`** (er+0x2701f3, AOB
  `48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48`). The ctor sets `*this = CS::CSHkDebugDisp::vftable`
  then `*this = hkDebugDisplayHandler::vftable` ⇒ **this class IS Havok's debug-display handler** — the exact
  "engine draws its own collision geometry from an internal handler" path (the community hitbox-viewer lead).
- Created inside the **draw-step init** (`FUN_1452701f3` ← `FUN_145b9062f` ← thunk @ er+0xc62e40; sibling init
  `FUN_140c62e1c` ← `FUN_1407aaaa1` er+0x7aaaa1, the 0x7a_ render-step region). So the collision-debug handler
  is instantiated in retail; drawing is gated by the Havok world's "debug display enabled" + per-shape flags.
- **In-engine ⇒ NO Havok-VDB version lock** (unlike `windows_havok_vdb_standup_re_prompt.md`). We already hold
  the `hknpWorld` (`CSPhysWorld+0x08`, `hknpworld_addbody_slot_re_findings.md`), so the enable is reachable.
- **Coverage:** collision only (walls' hulls, our `add_collision` bodies) — a FREE in-game collision view,
  directly useful for add_collision/greybox verification. Satisfies prompt Q3 "collision-only is still useful"
  but NOT job #2 "restyle the real render".

### 2. Debug DISPLAY step — `CSDbgDispStep` (entity/debug primitives)
- RTTI `.?AVCSDbgDispStep@CS@@` vtable **er+0x2bd1908**; execute-decider vt[0] **`FUN_14530a81b`** (er+0x530a81b,
  AOB `48 8B 05 F6 B2 A7 FE 48 85 C0` — the `mov rax,[rip+disp]` targets **`DAT_143d85b18`** (er+0x3d85b18) at
  match+3). It picks the work path `LAB_140791b47` when `DAT_143d85b18 != 0`, else the no-op
  `thunk_FUN_1407ab877`. ⇒ **`DAT_143d85b18` is a candidate run/enable gate for the debug-disp step** (probe
  it first). Its `_UpdateBudget` builds a `_ChrFinder_EnemyNum` list ⇒ this step draws **per-entity debug info**
  (names/ids/enemy counts), i.e. debug primitives, not a scene restyle.

## ❌ What was NOT found — a simple WHOLE-SCENE restyle flag (job #2 proper)
No single global that flips the world-mesh render to wireframe/untextured/flat surfaced in the debug-step or
render-manager walk. The debug steps ADD primitives/collision; they do not restyle the base geometry render
(FILL mode lives in immutable D3D12 PSOs). Unexplored candidate for a whole-scene style: the scene draw-param
managers **`CSWorldSceneDrawParamManager` / `CSFD4SceneDrawParam` / `CSFD4ModelDrawParam`** (RTTI present,
er+0x3ceb810 / 0x3cf3fc8 / 0x3cf2ba8) — a follow-up if the collision path isn't enough. Otherwise job #2 routes
to **(b) post-process shader** or **(c) D3D12 PSO `FILL_WIREFRAME` override**, as the prompt anticipated.

## ⛔ LIVE PROBE 2026-07-05 (Linux/Proton) — `DAT_143d85b18` is NOT the enable (it's a live context pointer)
Built + ran `dbgrender_probe` (RPC, `goblin_dbgrender.{hpp,cpp}`). **`DAT_143d85b18` @ er+0x3d85b18 reads
`0x…3d85c50` = a POINTER** (into the module, ≈0x138 past itself → `er+0x3d85c50`, a resident debug-context
global). So the `CSDbgDispStep` gate `if (DAT_143d85b18 != 0)` is **ALREADY satisfied every frame** — the step
already takes its work path — yet **no debug primitives are visible**. ⇒ `DAT_143d85b18` is a resident context
pointer, **not a bool enable**; the "probe it first" candidate is disproven as THE toggle. (The probe correctly
REFUSED to write 1 — that would have clobbered a live pointer → deref crash.)

**⇒ The real enable is DEEPER, not this gate.** Next candidates (need more RE, no cheap single-flip):
1. **Inside the work path `LAB_140791b47`** — a per-draw-type sub-flag (what actually turns on each primitive).
2. **The Havok COLLISION path (the stronger GO):** `CSHkDebugDisp`/`hkDebugDisplayHandler` — the enable is the
   hknpWorld's "debug display enabled" + per-shape flags, reachable from `CSPhysWorld+0x08` (we hold that via
   `goblin_add_collision.cpp`: `*inst → +0x98 CSPhysWorld → +0x08 hknpWorld`). Offset of the debug-display bool
   NOT yet pinned → the actionable next RE. A bounded live scan of the hknpWorld/handler region for a bool that,
   when set, makes collision hulls draw (with the add_collision box as the oracle) is the Linux-doable path;
   pinning it statically (Windows) is safer.
3. The retained **`CSDbgMenuStep`** (dev menu) — enabling it may expose the draw toggles as menu entries.

## ⛔ BOUNDED LINUX SCAN attempt 2026-07-05 — hit two walls, hand the exact enable to Windows RE
Extended `dbgrender_probe` (dump / findhk / poke8) for a bounded live hunt. Two blockers:
1. **`findhk` (a full-address-space scan for the `CSHkDebugDisp` vtable er+0x2b92230) FROZE the game** — it runs
   synchronously on the present/RPC thread, so the ~8 GB walk blocks the present loop → freeze (`alive=false`).
   Same failure class as the Windows agent's external-RPM global scan. A heap-object-by-vtable find is inherently
   a broad scan → not safe to run synchronously here.
2. The debug-disp **context @ er+0x3d85c50** (DAT_143d85b18's target) is a **pointer-heavy struct**
   (`0x…77d1ae0`, `0x…8985bf0`, a null qword, `0x…9442c40`), no obvious bool enable — and it's the entity
   debug-DISP context, NOT the Havok collision path. Blind byte-flipping in physics/debug structs = crash risk.

**⇒ The bounded Linux scan is disproven as a safe/high-confidence path.** The exact enable (the hknpWorld
"debug display enabled" bit / the `CSHkDebugDisp` handler enable / the work-path sub-flag) should be **pinned
STATICALLY in Ghidra (Windows)** — the agent already has the handler + step. Then the shipped Linux tooling
(`dbgrender_probe dump/poke8`, `goblin_dbgrender.cpp`) does a **surgical single-byte flip** at the RE'd offset
(safe, no scan). NB `findhk` is a dev footgun (freezes) — keep it off unless made async/bounded.

## Verdict
- **Collision wireframe / in-game collision view → GO-likely** via the retained `CSHkDebugDisp`
  (`hkDebugDisplayHandler`) enable + `CSDbgDispStep`'s `DAT_143d85b18` gate. In-engine, no VDB version lock,
  reuses our live `hknpWorld`. **This is the cheap greybox-adjacent win** and directly serves the collision/
  add_collision verification goal.
- **Whole-scene "restyle the real render" (job #2) → NO simple flag found.** Route to SceneDrawParam recon
  (secondary) or job #2(b)/(c). This closes the "is there a free full-restyle switch?" question: no.

## Live probe plan (acceptance — for the game-driving side; Proton/Linux or Windows)
Behind a throwaway `dbgrender_probe` RPC (mirror `goblin_geom_move.cpp` SEH read/write + the existing
`screenshot` RPC):
1. `read` the candidates: `DAT_143d85b18` (er+0x3d85b18), and the Havok world's debug-display-handler pointer
   / per-shape debug flag reachable from `CSPhysWorld+0x08`.
2. `set` each under SEH, then `screenshot`; stand in a lit textured area facing a wall whose collision differs
   from its mesh (oracle). Look for collision hulls / debug primitives appearing; verify the game stays
   interactive (step, swing → no freeze). Revert + screenshot to confirm a clean toggle.
3. Record value→effect + the safe write thread/timing. A screenshot pair (off/on) with the engine drawing its
   own collision geometry, game alive = the confirmed GO.

## Anchors (er-relative, imagebase 0x140000000)
- Step tree ctor `FUN_140ded060` (CSSystemStep; installs the debug child steps).
- `CSDbgDispStep` vtable er+0x2bd1908; execute-decider `FUN_14530a81b` (er+0x530a81b) → gate `DAT_143d85b18`
  (er+0x3d85b18); work path `LAB_140791b47`, skip path `FUN_1407ab877`.
- `CSHkDebugDisp`/`hkDebugDisplayHandler` vtable er+0x2b92230; ctor `FUN_1452701f3` (er+0x2701f3); created via
  `FUN_145b9062f` / `FUN_140c62e1c` ← `FUN_1407aaaa1` (er+0x7aaaa1).
- SceneDrawParam managers (whole-scene follow-up): `CSWorldSceneDrawParamManager` er+0x3ceb810,
  `CSFD4SceneDrawParam` er+0x3cf3fc8, `CSFD4ModelDrawParam` er+0x3cf2ba8.
